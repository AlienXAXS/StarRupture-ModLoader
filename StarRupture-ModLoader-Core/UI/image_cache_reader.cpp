#include "UI/image_cache_reader.h"
#include "UI/image_cache_care.h"
#include "UI/image_resample.h"
#include "UI/image_shrink.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdio>

namespace ImageCache
{
	namespace
	{
		std::wstring Join(const std::wstring& folder, const std::string& name)
		{
			std::wstring path = folder;
			if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
				path += L'\\';
			path.append(name.begin(), name.end());
			return path;
		}

		// Busy: the file could not be opened or read now (a scanner has it); it is not at fault.
		// TooBig: it is not one of ours.
		enum class ReadState { Ok, Missing, Busy, TooBig };

		// The whole file, or why not.
		ReadState ReadAll(const std::wstring& path, uint64_t limit, std::vector<uint8_t>& bytes)
		{
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				const DWORD error = GetLastError();
				return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ReadState::Missing : ReadState::Busy;
			}

			LARGE_INTEGER size = {};
			ReadState state = ReadState::Busy;
			if (GetFileSizeEx(file, &size) && size.QuadPart >= 0)
			{
				if (static_cast<uint64_t>(size.QuadPart) > limit)
					state = ReadState::TooBig;
				else
				{
					bytes.resize(static_cast<size_t>(size.QuadPart));
					DWORD read = 0;
					if (bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()))
						state = ReadState::Ok;
				}
			}
			CloseHandle(file);
			return state;
		}

		bool IsImageFileName(const wchar_t* name)
		{
			// 16 hex digits and ".img".
			if (wcslen(name) != 20 || wcscmp(name + 16, L".img") != 0)
				return false;
			for (int i = 0; i < 16; ++i)
				if (!iswxdigit(name[i]))
					return false;
			return true;
		}
	}

	Reader::Reader(Session session, Uploader& uploader, Log log)
		: m_session(std::move(session)), m_uploader(uploader), m_log(std::move(log))
	{
	}

	Reader::~Reader()
	{
		Stop();
	}

	Reader::State Reader::Poll(uint64_t nowMs)
	{
		if (m_state == State::Off)
			return m_state;

		const FolderFn folder = m_session ? m_session() : FolderFn();
		if (!folder)
		{
			m_state = State::Off;
			if (m_log)
				m_log("[Images] Images are not read from the cache folder this session");
			return m_state;
		}

		if (!m_opening)
		{
			m_opening    = true;
			m_openedAtMs = nowMs;
			Job job;
			job.open   = true;
			job.folder = folder;
			if (!Enqueue(std::move(job)))
				m_state = State::Off;
			return m_state;
		}

		if (m_state == State::Loading)
		{
			bool opened;
			{
				std::lock_guard<std::mutex> lock(m_lock);
				opened = m_opened;
			}
			if (opened)
				m_state = State::Ready;
			else if (nowMs - m_openedAtMs >= kOpenWaitMs)
			{
				m_state = State::Off;
				if (m_log)
					m_log("[Images] The cache folder was not listed in time: images are not read from it this session");
			}
		}
		return m_state;
	}

	bool Reader::Has(const std::string& key)
	{
		if (m_state != State::Ready)
			return false;
		if (IsTransientPath(key))
			return false;
		const std::string name = FileName(key);
		std::lock_guard<std::mutex> lock(m_lock);
		return m_index.count(name) != 0;
	}

	bool Reader::Request(PluginImageId id, const std::string& key)
	{
		if (m_state != State::Ready)
			return false;
		Job job;
		job.id  = id;
		job.key = key;
		return Enqueue(std::move(job));
	}

	ImageDisk::Source::ExactAnswer Reader::RequestExact(PluginImageId id, const std::string& key, uint32_t size)
	{
		if (m_state == State::Off || size == 0 || IsTransientPath(key))
			return ExactAnswer::Refused;
		if (m_state != State::Ready)
			return ExactAnswer::Busy;
		Job job;
		job.id        = id;
		job.key       = key;
		job.exactSize = size;
		return Enqueue(std::move(job)) ? ExactAnswer::Started : ExactAnswer::Busy;
	}

	void Reader::Collect(std::vector<ImageDisk::FileResult>& out)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (!m_abandoned)
			out.insert(out.end(), m_results.begin(), m_results.end());
		m_results.clear();
	}

	bool Reader::TakeLists(ImageDisk::Lists& out)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (!m_opened || m_listsTaken)
			return false;
		m_listsTaken = true;
		out = std::move(m_lists);
		return true;
	}

	void Reader::Abandon()
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_abandoned = true;
		m_results.clear();
	}

	void Reader::Stop()
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

	bool Reader::Enqueue(Job&& job)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (m_stop || m_queue.size() >= kMaxQueued)
			return false;
		if (!m_thread.joinable())
			m_thread = std::thread(&Reader::Run, this);
		m_queue.push_back(std::move(job));
		m_wake.notify_one();
		return true;
	}

	void Reader::Run()
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
			if (job.open)
				Open(job);
			else if (job.exactSize != 0)
				LoadExact(job);
			else
				Load(job);
		}
	}

	// Lists the folder and reads the two lists in it. A list that is missing or does not check
	// out is left empty: the saver writes a new one.
	void Reader::Open(const Job& job)
	{
		m_folder = job.folder();
		if (m_folder.empty())
		{
			// No folder for this session's key: nothing to list.
			std::lock_guard<std::mutex> lock(m_lock);
			m_opened = true;
			return;
		}

		std::unordered_set<std::string> index;
		WIN32_FIND_DATAW found = {};
		HANDLE find = FindFirstFileExW(Join(m_folder, "*.img").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImageFileName(found.cFileName))
				{
					std::string name;
					for (const wchar_t* c = found.cFileName; *c; ++c)
						name += static_cast<char>(towlower(*c));
					index.insert(std::move(name));
				}
			} while (FindNextFileW(find, &found));
			FindClose(find);
		}

		ImageDisk::Lists lists;
		std::vector<uint8_t> bytes;
		if (ReadAll(Join(m_folder, NameListFileName()), kMaxFileBytes, bytes) == ReadState::Ok)
			if (ParseNames(bytes.data(), bytes.size(), lists.names) != Parsed::Ok)
				lists.names.clear();
		bytes.clear();
		if (ReadAll(Join(m_folder, SetListFileName()), kMaxFileBytes, bytes) == ReadState::Ok)
			if (ParseSets(bytes.data(), bytes.size(), lists.sets) != Parsed::Ok)
				lists.sets.clear();

		// Saved names and set entries for run-time objects are not used, and their files go.
		const auto dropTransient = [&](const std::string& path)
		{
			if (!IsTransientPath(path))
				return false;
			DeleteFileW(Join(m_folder, FileName(path)).c_str());
			index.erase(FileName(path));
			return true;
		};
		lists.names.erase(std::remove_if(lists.names.begin(), lists.names.end(),
			[&](const NamePair& pair) { return dropTransient(pair.path); }), lists.names.end());
		for (SetEntry& entry : lists.sets)
			entry.paths.erase(std::remove_if(entry.paths.begin(), entry.paths.end(), dropTransient), entry.paths.end());

		std::lock_guard<std::mutex> lock(m_lock);
		m_index  = std::move(index);
		m_lists  = std::move(lists);
		m_opened = true;
	}

	// Reads, checks and uploads one file. Whatever comes of it, the image is forgotten by the
	// listing, so the game thread reads the game for it from then on.
	void Reader::Load(const Job& job)
	{
		const std::wstring path = Join(m_folder, FileName(job.key));

		// A run-time object's name stands for another texture next session: its file is not used.
		if (IsTransientPath(job.key))
		{
			DeleteFileW(path.c_str());
			Finish(job.id, job.key, ImageDisk::FileOutcome::Failed, nullptr, 0);
			return;
		}

		std::vector<uint8_t> bytes;
		switch (ReadAll(path, kMaxFileBytes, bytes))
		{
		case ReadState::Ok:
			break;
		case ReadState::Missing:
			Finish(job.id, job.key, ImageDisk::FileOutcome::Failed, nullptr, 0);
			return;
		case ReadState::Busy:
			// Held by something else just now: a good file is not deleted for that.
			Finish(job.id, job.key, ImageDisk::FileOutcome::Failed, nullptr, 0);
			return;
		case ReadState::TooBig:
			Reject(path, job.key, "too big");
			Finish(job.id, job.key, ImageDisk::FileOutcome::Rejected, nullptr, 0);
			return;
		}

		Info info;
		const uint8_t* pixels = nullptr;
		if (const char* why = Check(bytes, job.key, info, pixels))
		{
			Reject(path, job.key, why);
			Finish(job.id, job.key, ImageDisk::FileOutcome::Rejected, nullptr, 0);
			return;
		}

		const size_t pixelBytes = static_cast<size_t>(ImageShrink::Bytes({ info.storedWidth, info.storedHeight }, info.levels));
		PluginTextureHandle texture = nullptr;
		if (!m_uploader.Upload(info.storedWidth, info.storedHeight, info.levels, pixels, pixelBytes, texture) || !texture)
		{
			Finish(job.id, job.key, ImageDisk::FileOutcome::Failed, nullptr, 0);
			return;
		}
		Care::Touch(path);   // in use again: the size cap deletes the least recently used first
		Finish(job.id, job.key, ImageDisk::FileOutcome::Texture, texture, pixelBytes);
	}

	// Why the bytes are not the file of the image the key names, or null when they are.
	const char* Reader::Check(const std::vector<uint8_t>& bytes, const std::string& key, Info& info, const uint8_t*& pixels) const
	{
		const Parsed parsed = Parse(bytes.data(), bytes.size(), info, pixels);
		if (parsed != Parsed::Ok)
			return Describe(parsed);
		if (ImageLibrary::NormalizeObjectPath(info.path.c_str()) != key)
			return "holds another image";
		if (info.storedWidth > ImageLibrary::kStoredSize || info.storedHeight > ImageLibrary::kStoredSize)
			return "wrong size";
		return nullptr;
	}

	// Makes the copy of one file's picture for one size. The file and the listing are left alone
	// whatever happens: the stored picture's own load is what rejects a file that is no good.
	void Reader::LoadExact(const Job& job)
	{
		const std::wstring path = Join(m_folder, FileName(job.key));
		const auto end = [&](ImageDisk::FileOutcome outcome, PluginTextureHandle texture, uint64_t bytes, uint32_t width, uint32_t height)
		{
			FinishExact(job.id, job.exactSize, outcome, texture, bytes, width, height);
		};

		std::vector<uint8_t> bytes;
		if (m_folder.empty() || ReadAll(path, kMaxFileBytes, bytes) != ReadState::Ok)
		{
			end(ImageDisk::FileOutcome::Failed, nullptr, 0, 0, 0);
			return;
		}
		Info info;
		const uint8_t* pixels = nullptr;
		if (Check(bytes, job.key, info, pixels))
		{
			end(ImageDisk::FileOutcome::Failed, nullptr, 0, 0, 0);
			return;
		}

		// A size the stored picture does not exceed is drawn from the stored picture.
		const ImageShrink::Size size = ImageShrink::StoredSize(info.storedWidth, info.storedHeight, job.exactSize);
		if (size.width == info.storedWidth && size.height == info.storedHeight)
		{
			end(ImageDisk::FileOutcome::Same, nullptr, 0, 0, 0);
			return;
		}

		std::vector<uint8_t> copy;
		ImageResample::Resample(pixels, info.storedWidth, info.storedHeight, size.width, size.height, copy);
		PluginTextureHandle texture = nullptr;
		if (copy.empty() || !m_uploader.Upload(size.width, size.height, 1, copy.data(), copy.size(), texture) || !texture)
		{
			end(ImageDisk::FileOutcome::Failed, nullptr, 0, 0, 0);
			return;
		}
		end(ImageDisk::FileOutcome::Texture, texture, copy.size(), size.width, size.height);
	}

	void Reader::FinishExact(PluginImageId id, uint32_t size, ImageDisk::FileOutcome outcome, PluginTextureHandle texture,
		uint64_t bytes, uint32_t width, uint32_t height)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		ImageDisk::FileResult result;
		result.id        = id;
		result.outcome   = outcome;
		result.texture   = texture;
		result.bytes     = bytes;
		result.exactSize = size;
		result.width     = width;
		result.height    = height;
		m_results.push_back(result);
	}

	void Reader::Finish(PluginImageId id, const std::string& key, ImageDisk::FileOutcome outcome, PluginTextureHandle texture, uint64_t bytes)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (outcome != ImageDisk::FileOutcome::Texture)
			m_index.erase(FileName(key));
		ImageDisk::FileResult result;
		result.id      = id;
		result.outcome = outcome;
		result.texture = texture;
		result.bytes   = bytes;
		m_results.push_back(result);
	}

	void Reader::Reject(const std::wstring& path, const std::string& key, const char* why)
	{
		DeleteFileW(path.c_str());
		++m_rejected;
		if (m_log)
		{
			char line[256];
			snprintf(line, sizeof(line), "[Images] A cache file was rejected (%s) and will be read from the game again: %.120s", why, key.c_str());
			m_log(line);
		}
	}
}
