#include "UI/image_cache_writer.h"
#include "UI/image_cache_care.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace ImageCache
{
	namespace
	{
		// Creates the folder and every missing parent. Returns 0 or the Windows error.
		DWORD MakeFolders(const std::wstring& folder)
		{
			if (CreateDirectoryW(folder.c_str(), nullptr))
				return 0;
			DWORD error = GetLastError();
			if (error == ERROR_ALREADY_EXISTS)
				return 0;
			if (error != ERROR_PATH_NOT_FOUND)
				return error;

			const size_t cut = folder.find_last_of(L"\\/");
			if (cut == std::wstring::npos || cut == 0)
				return error;
			if (const DWORD parent = MakeFolders(folder.substr(0, cut)))
				return parent;
			if (CreateDirectoryW(folder.c_str(), nullptr))
				return 0;
			error = GetLastError();
			return error == ERROR_ALREADY_EXISTS ? 0 : error;
		}

		bool SameSizeExists(const std::wstring& path, uint64_t size)
		{
			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				return false;
			return ((static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow) == size;
		}

		// The errors that say the folder cannot be written to at all.
		bool FolderUnusable(DWORD error)
		{
			switch (error)
			{
			case ERROR_WRITE_PROTECT:
			case ERROR_DISK_FULL:
			case ERROR_HANDLE_DISK_FULL:
			case ERROR_PATH_NOT_FOUND:
			case ERROR_INVALID_NAME:
			case ERROR_DIRECTORY:
			case ERROR_FILENAME_EXCED_RANGE:
				return true;
			default:
				return false;
			}
		}

		// The errors one file meets when something else holds it: a virus scanner on the temp file,
		// a reader on the file being replaced, a read-only file in the way.
		bool FileBusy(DWORD error)
		{
			switch (error)
			{
			case ERROR_ACCESS_DENIED:
			case ERROR_SHARING_VIOLATION:
			case ERROR_LOCK_VIOLATION:
			case ERROR_FILE_READ_ONLY:
			case ERROR_DELETE_PENDING:
				return true;
			default:
				return false;
			}
		}

		constexpr int   kAttempts  = 3;
		constexpr DWORD kBackoffMs = 25;   // before the second try; twice that before the third

		// Writes temp, then renames it over path. Returns 0 or the Windows error; no temp is left.
		DWORD WriteThenRename(const std::wstring& temp, const std::wstring& path, const std::vector<uint8_t>& bytes)
		{
			HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return GetLastError();

			DWORD wrote = 0;
			bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size();
			DWORD error = ok ? 0 : GetLastError();
			CloseHandle(file);
			if (ok && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				ok = false;
				error = GetLastError();
			}
			if (!ok)
				DeleteFileW(temp.c_str());
			return error;
		}

		// Whether a new file can be made in the folder: 0, or the Windows error.
		DWORD ProbeFolder(const std::wstring& folder)
		{
			std::wstring probe = folder;
			if (probe.back() != L'\\' && probe.back() != L'/')
				probe += L'\\';
			probe += L"probe." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
			HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return GetLastError();
			CloseHandle(file);
			return 0;
		}

		std::string ToUtf8(const std::wstring& text)
		{
			if (text.empty())
				return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
			if (size > 0)
				WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}
	}

	Writer::Writer(Log log, size_t maxQueued)
		: m_log(std::move(log)), m_maxQueued(maxQueued)
	{
	}

	Writer::~Writer()
	{
		Stop();
	}

	bool Writer::Submit(Job&& job)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (m_stop || (m_off && !job.runWhenOff) || m_queue.size() >= m_maxQueued || !job.make || !job.folder)
			return false;
		if (!m_thread.joinable())
			m_thread = std::thread(&Writer::Run, this);
		m_queue.push_back(std::move(job));
		m_wake.notify_one();
		return true;
	}

	bool Writer::Off() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_off;
	}

	void Writer::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_stop = true;
			m_queue.clear();
			m_wake.notify_all();
		}
		if (m_thread.joinable())
			m_thread.join();
	}

	void Writer::Run()
	{
		for (;;)
		{
			Job job;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
				if (m_stop)
					return;
				job = std::move(m_queue.front());
				m_queue.pop_front();
			}
			if (!Write(job))
			{
				std::lock_guard<std::mutex> lock(m_lock);
				m_off = true;
				// What is queued is dropped, except the jobs that only delete (Clear).
				std::deque<Job> kept;
				for (Job& queued : m_queue)
					if (queued.runWhenOff)
						kept.push_back(std::move(queued));
				m_queue = std::move(kept);
			}
		}
	}

	void Writer::SwitchOff(const char* step, unsigned long error, const std::wstring& folder)
	{
		if (!m_log)
			return;
		m_log(std::string("[Images] The image cache folder cannot be written (") + step + ", error " + std::to_string(error)
			+ ", " + ToUtf8(folder) + "): images are not saved this session");
	}

	bool Writer::Write(const Job& job)
	{
		const std::wstring folder = job.folder();
		if (folder.empty())
			return true;   // the folder's key is not the session's any more: nothing is filed under it

		const std::vector<uint8_t> bytes = job.make();
		if (bytes.empty())
			return true;

		const std::wstring name(job.name.begin(), job.name.end());
		std::wstring path = folder;
		if (path.back() != L'\\' && path.back() != L'/')
			path += L'\\';
		path += name;

		if (!job.replace && SameSizeExists(path, bytes.size()))
		{
			Care::Touch(path);   // in use again: the size cap deletes the least recently used first
			return true;
		}

		if (const DWORD error = MakeFolders(folder))
		{
			SwitchOff("create the folder", error, folder);
			return false;
		}

		const std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
		for (int attempt = 0; attempt < kAttempts; ++attempt)
		{
			const DWORD error = WriteThenRename(temp, path, bytes);
			if (!error)
			{
				++m_written;
				return true;
			}
			if (FolderUnusable(error))
			{
				SwitchOff("write a file", error, folder);
				return false;
			}
			if (!FileBusy(error))
				return true;   // this one file is skipped
			if (attempt + 1 < kAttempts)
				Sleep(kBackoffMs * static_cast<DWORD>(attempt + 1));
		}

		// The file stayed busy. If the folder still takes a new file, only this one is held and it
		// is skipped; if not, the folder is closed to us.
		if (const DWORD error = ProbeFolder(folder))
		{
			SwitchOff("create a file", error, folder);
			return false;
		}
		return true;
	}
}
