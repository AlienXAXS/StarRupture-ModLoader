#include "UI/image_cache_care.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace ImageCache::Care
{
	namespace
	{
		struct Entry
		{
			std::wstring name;
			bool         folder = false;
			uint64_t     size   = 0;
			uint64_t     time   = 0;   // last write, FILETIME ticks
		};

		uint64_t Join(DWORD high, DWORD low)
		{
			return (static_cast<uint64_t>(high) << 32) | low;
		}

		std::wstring Trim(std::wstring path)
		{
			while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
				path.pop_back();
			return path;
		}

		std::wstring Leaf(const std::wstring& path)
		{
			const std::wstring trimmed = Trim(path);
			const size_t cut = trimmed.find_last_of(L"\\/");
			return cut == std::wstring::npos ? trimmed : trimmed.substr(cut + 1);
		}

		bool SameName(const std::wstring& a, const std::wstring& b)
		{
			return _wcsicmp(a.c_str(), b.c_str()) == 0;
		}

		bool EndsWith(const std::wstring& text, const wchar_t* tail)
		{
			const size_t n = wcslen(tail);
			return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, tail) == 0;
		}

		// The files and folders directly inside dir. A link is left out, so nothing outside the
		// cache is ever followed.
		std::vector<Entry> List(const std::wstring& dir)
		{
			std::vector<Entry> entries;
			WIN32_FIND_DATAW data;
			HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
			if (find == INVALID_HANDLE_VALUE)
				return entries;
			do
			{
				const std::wstring name = data.cFileName;
				if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
					continue;
				Entry entry;
				entry.name   = name;
				entry.folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				entry.size   = Join(data.nFileSizeHigh, data.nFileSizeLow);
				entry.time   = Join(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
				entries.push_back(std::move(entry));
			} while (FindNextFileW(find, &data));
			FindClose(find);
			return entries;
		}

		bool DeleteOne(const std::wstring& path, uint64_t size, Result& result)
		{
			if (!DeleteFileW(path.c_str()))
			{
				if (GetLastError() != ERROR_ACCESS_DENIED)
					return false;
				// A read-only file; one that is open elsewhere still refuses.
				SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
				if (!DeleteFileW(path.c_str()))
					return false;
			}
			++result.filesRemoved;
			result.bytesFreed += size;
			return true;
		}

		// Everything inside dir, then dir itself. Returns whether dir is gone.
		bool DeleteTree(const std::wstring& dir, Result& result)
		{
			for (const Entry& entry : List(dir))
			{
				const std::wstring path = dir + L"\\" + entry.name;
				if (entry.folder)
					DeleteTree(path, result);
				else
					DeleteOne(path, entry.size, result);
			}
			return RemoveDirectoryW(dir.c_str()) != 0;
		}

		// "<name>.<pid>.tmp" is what the writer makes. False for any other name.
		bool TempOwner(const std::wstring& name, unsigned long& pid)
		{
			if (!EndsWith(name, L".tmp"))
				return false;
			const std::wstring stem = name.substr(0, name.size() - 4);
			const size_t dot = stem.find_last_of(L'.');
			if (dot == std::wstring::npos || dot + 1 >= stem.size() || stem.size() - dot - 1 > 10)
				return false;
			unsigned long long value = 0;
			for (size_t i = dot + 1; i < stem.size(); ++i)
			{
				if (stem[i] < L'0' || stem[i] > L'9')
					return false;
				value = value * 10 + static_cast<unsigned>(stem[i] - L'0');
			}
			if (value == 0 || value > 0xFFFFFFFFull)
				return false;
			pid = static_cast<unsigned long>(value);
			return true;
		}

		void RemoveLeftovers(const std::wstring& folder, Result& result)
		{
			for (const Entry& entry : List(folder))
			{
				unsigned long pid = 0;
				if (entry.folder || !TempOwner(entry.name, pid) || ProcessAlive(pid))
					continue;
				if (DeleteOne(folder + L"\\" + entry.name, entry.size, result))
					++result.leftoversRemoved;
			}
		}

		struct CapFile
		{
			std::wstring path;
			uint64_t     size = 0;
			uint64_t     time = 0;
			bool         image = false;
			bool         older = false;   // in the kept folder that is not the current one
		};

		void TrimToCap(const std::vector<std::pair<std::wstring, bool>>& kept, uint64_t capBytes, Result& result)
		{
			std::vector<CapFile> files;
			uint64_t total = 0;
			for (const auto& [folder, older] : kept)
			{
				for (const Entry& entry : List(folder))
				{
					if (entry.folder)
						continue;
					total += entry.size;
					files.push_back({ folder + L"\\" + entry.name, entry.size, entry.time, EndsWith(entry.name, L".img"), older });
				}
			}
			if (total <= capBytes)
				return;

			// The older folder first, then by age; the set list and any stray file are never picked.
			std::stable_sort(files.begin(), files.end(), [](const CapFile& a, const CapFile& b)
			{
				if (a.older != b.older)
					return a.older;
				return a.time < b.time;
			});
			for (const CapFile& file : files)
			{
				if (total <= capBytes)
					break;
				if (file.image && DeleteOne(file.path, file.size, result))
					total -= file.size;
			}
		}
	}

	bool ProcessAlive(unsigned long pid)
	{
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
		if (!process)
			return GetLastError() != ERROR_INVALID_PARAMETER;   // refused: it exists, we just cannot look
		const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		CloseHandle(process);
		return alive;
	}

	void Touch(const std::wstring& file)
	{
		HANDLE handle = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return;
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		SetFileTime(handle, nullptr, nullptr, &now);
		CloseHandle(handle);
	}

	Result Tidy(const std::wstring& rootPath, const std::wstring& currentPath, uint64_t capBytes)
	{
		Result result;
		const std::wstring root = Trim(rootPath);
		const std::wstring current = Leaf(currentPath);
		// Without a current folder there is nothing to keep it by, and the one in use could be deleted.
		if (root.empty() || current.empty())
			return result;

		// A folder is as new as its newest file: deleting from a folder (or finding a file in
		// it busy) changes the folder's own time, but not that.
		std::vector<Entry> folders;
		for (Entry& entry : List(root))
		{
			if (!entry.folder)
				continue;
			entry.time = 0;
			for (const Entry& file : List(root + L"\\" + entry.name))
			{
				if (!file.folder)
					entry.time = std::max(entry.time, file.time);
			}
			folders.push_back(std::move(entry));
		}
		std::stable_sort(folders.begin(), folders.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });

		// The current folder, and the newest of the others.
		std::vector<std::pair<std::wstring, bool>> kept;
		bool haveOther = false;
		for (const Entry& folder : folders)
		{
			const std::wstring path = root + L"\\" + folder.name;
			if (SameName(folder.name, current))
				kept.push_back({ path, false });
			else if (!haveOther)
			{
				haveOther = true;
				kept.push_back({ path, true });
			}
			else if (DeleteTree(path, result))
				++result.foldersRemoved;
		}

		for (const auto& folder : kept)
			RemoveLeftovers(folder.first, result);
		TrimToCap(kept, capBytes, result);
		return result;
	}

	Result Clear(const std::wstring& rootPath)
	{
		Result result;
		const std::wstring root = Trim(rootPath);
		if (root.empty())
			return result;
		for (const Entry& entry : List(root))
		{
			const std::wstring path = root + L"\\" + entry.name;
			if (!entry.folder)
				DeleteOne(path, entry.size, result);
			else if (DeleteTree(path, result))
				++result.foldersRemoved;
		}
		return result;
	}

	namespace
	{
		void Say(const Log& log, const char* what, const Result& result)
		{
			if (!log)
				return;
			char line[256];
			snprintf(line, sizeof(line), "[Images] Image cache %s: %u files removed, %.1f MB freed",
				what, result.filesRemoved, static_cast<double>(result.bytesFreed) / (1024.0 * 1024.0));
			log(line);
		}
	}

	// The work runs inside `make`, which the worker calls; it returns no bytes, so the writer
	// has nothing to write for the job.
	Job TidyJob(std::function<std::wstring()> root, std::function<std::wstring()> current, Log log)
	{
		Job job;
		job.folder = root;
		job.name   = "tidy";
		job.make   = [root, current, log]() -> std::vector<uint8_t>
		{
			const Result result = Tidy(root(), current ? current() : std::wstring());   // no current folder: nothing is touched
			if (result.filesRemoved || result.foldersRemoved)
				Say(log, "tidied", result);
			return {};
		};
		return job;
	}

	Job ClearJob(std::function<std::wstring()> root, Log log)
	{
		Job job;
		job.folder = root;
		job.name   = "clear";
		job.runWhenOff = true;
		job.make   = [root, log]() -> std::vector<uint8_t>
		{
			Say(log, "cleared", Clear(root()));
			return {};
		};
		return job;
	}
}
