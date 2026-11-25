#include "auto_update/thumbnail_fetcher.h"

#include "auto_update/thumbnail_check.h"
#include "logging/logger.h"
#include "memory_scanner/version_feed.h"
#include "utils/sha256.h"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ModIndex
{
	namespace
	{
		struct Item
		{
			ThumbnailInfo info;
			ThumbnailState state = ThumbnailState::Loading;
			std::string bytes;           // Bytes only
			uint64_t lastWant = 0;       // the clock's ms of the latest Want
		};

		constexpr unsigned kPausePollMs = 250;
		constexpr char kHost[] = "https://raw.githubusercontent.com/";

		uint64_t SteadyNowMs()
		{
			using namespace std::chrono;
			return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}

		// The host of an address, for the log: never the path or query.
		std::string HostOf(const std::string& url)
		{
			const size_t start = url.compare(0, 8, "https://") == 0 ? 8 : std::string::npos;
			if (start == std::string::npos)
				return "an address that is not a plain https one";
			const size_t end = url.find_first_of("/?#", start);
			const std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
			for (const char c : host)
				if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F || c == '%' || c == '@')
					return "an address that is not a plain https one";
			return host;
		}

		// A list entry the loader will fetch for: the parser's guarantees, checked again
		// here because this is where a name on disk and a request are made from them.
		bool IsFetchable(const ThumbnailInfo& info)
		{
			return info.url.compare(0, sizeof(kHost) - 1, kHost) == 0 && IsThumbnailHash(info.sha256)
				&& info.size >= 1 && info.size <= kMaxThumbnailBytes;
		}

		int64_t UnixSeconds(const FILETIME& ft)
		{
			ULARGE_INTEGER t;
			t.LowPart = ft.dwLowDateTime;
			t.HighPart = ft.dwHighDateTime;
			return static_cast<int64_t>(t.QuadPart / 10000000ULL) - 11644473600LL;
		}
	}

	struct ThumbnailFetcher::Shared
	{
		mutable std::mutex mutex;
		std::condition_variable changed;          // work arrived, stop was set, or the worker ended
		ThumbnailFetcherConfig config;
		std::atomic<bool> cancel{ false };
		std::map<std::string, Item> items;
		std::deque<std::string> queue;            // ids in the order they were wanted
		std::set<std::string> known;              // the hashes of the current list
		ThumbnailBudget budget;
		bool listSigned = false;
		bool tidied = false;
		bool running = false;                     // a worker thread is alive
		bool stopped = false;

		uint64_t Now() const { return config.nowMs ? config.nowMs() : SteadyNowMs(); }
	};

	namespace
	{
		struct Outcome
		{
			ThumbnailState state = ThumbnailState::Failed;
			std::string bytes;
		};

		// The cache file of `info`, if it is there and still the picture the list
		// describes. A file that is not is deleted.
		bool ReadCached(const std::wstring& path, const std::string& label, const ThumbnailInfo& info, std::string& bytes)
		{
			bool missing = false;
			if (VersionFeed::ReadFileCapped(path, bytes, missing))
			{
				if (bytes.size() == info.size && Sha256::OfBytes(bytes) == info.sha256
					&& CheckThumbnailHeader(bytes) == ThumbnailCheck::Ok)
				{
					ModLoaderLogger::LogDebug(L"[Thumbs] %hs: cached picture used", label.c_str());
					return true;
				}
				ModLoaderLogger::LogWarn(L"[Thumbs] %hs: the cached picture does not match the list; deleted", label.c_str());
			}
			else if (!missing)
			{
				ModLoaderLogger::LogWarn(L"[Thumbs] %hs: the cached picture could not be read; deleted", label.c_str());
			}
			else
			{
				return false;
			}
			bytes.clear();
			DeleteFileW(path.c_str());
			return false;
		}

		// Steps 2 to 5 of the header: the download, the checks, the cache file.
		Outcome Download(ThumbnailFetcher::Shared& s, const std::string& label, const ThumbnailInfo& info, const std::wstring& path)
		{
			Outcome out;
			{
				std::lock_guard<std::mutex> lock(s.mutex);
				if (!s.budget.Reserve(info.size))
				{
					ModLoaderLogger::LogWarn(L"[Thumbs] %hs: not downloaded, the session limit of %zu pictures or %zu bytes is reached",
						label.c_str(), s.config.maxDownloads, s.config.maxSessionBytes);
					return out;
				}
			}

			const std::string host = HostOf(info.url);
			ModInstall::Hop hop = s.config.hop(info.url, info.size, s.config.deadlineMs, s.cancel);
			if (s.cancel.load())
				return out;
			if (hop.status != 200)
			{
				if (hop.status >= 300 && hop.status < 400)
					ModLoaderLogger::LogWarn(L"[Thumbs] %hs: %hs sent a redirect to %hs; not followed",
						label.c_str(), host.c_str(), HostOf(hop.location).c_str());
				else
					ModLoaderLogger::LogWarn(L"[Thumbs] %hs: download from %hs failed (status %u)",
						label.c_str(), host.c_str(), hop.status);
				return out;
			}

			std::string& body = hop.body;
			if (body.size() != info.size)
			{
				ModLoaderLogger::LogWarn(L"[Thumbs] %hs: got %zu bytes from %hs, expected %zu; nothing written",
					label.c_str(), body.size(), host.c_str(), info.size);
				out.state = body.empty() ? ThumbnailState::Failed : ThumbnailState::Mismatch;
				return out;
			}
			const std::string digest = Sha256::OfBytes(body);
			if (digest != info.sha256)
			{
				ModLoaderLogger::LogWarn(L"[Thumbs] %hs: SHA-256 mismatch from %hs: expected %hs, got %hs; nothing written",
					label.c_str(), host.c_str(), info.sha256.c_str(), digest.c_str());
				out.state = ThumbnailState::Mismatch;
				return out;
			}
			unsigned width = 0, height = 0;
			const ThumbnailCheck check = CheckThumbnailHeader(body, &width, &height);
			if (check != ThumbnailCheck::Ok)
			{
				ModLoaderLogger::LogWarn(L"[Thumbs] %hs: not a usable picture (%hs, %ux%u); nothing written", label.c_str(),
					check == ThumbnailCheck::NotPng ? "not a PNG" : "too large", width, height);
				return out;
			}

			// Verified. The cache file is for next time: if it cannot be written the
			// picture is still shown now.
			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				CreateDirectoryW(s.config.cacheDir.c_str(), nullptr);
				std::string why;
				if (!VersionFeed::WriteFileAtomic(path, body, why))
					ModLoaderLogger::LogWarn(L"[Thumbs] %hs: not cached: %hs", label.c_str(), why.c_str());
			}
			ModLoaderLogger::LogInfo(L"[Thumbs] %hs: downloaded %zu bytes (%ux%u), SHA-256 matches", label.c_str(), body.size(), width, height);
			out.state = ThumbnailState::Bytes;
			out.bytes = std::move(body);
			return out;
		}

		Outcome Fetch(ThumbnailFetcher::Shared& s, const std::string& label, const ThumbnailInfo& info)
		{
			const std::wstring path = ThumbnailCachePath(s.config.cacheDir, info.sha256);
			Outcome out;
			if (!path.empty() && ReadCached(path, label, info, out.bytes))
			{
				out.state = ThumbnailState::Bytes;
				return out;
			}
			return Download(s, label, info, path);
		}

		void Work(std::shared_ptr<ThumbnailFetcher::Shared> shared)
		{
			ThumbnailFetcher::Shared& s = *shared;
			for (;;)
			{
				std::set<std::string> tidyList;
				bool tidy = false;
				{
					std::lock_guard<std::mutex> lock(s.mutex);
					if (s.stopped || s.queue.empty())
					{
						s.running = false;
						s.changed.notify_all();
						return;
					}
					if (!s.tidied && s.listSigned)
					{
						s.tidied = true;
						tidy = true;
						tidyList = s.known;
					}
				}

				if (tidy)
				{
					try
					{
						const size_t gone = TidyThumbnailCache(s.config.cacheDir, tidyList, static_cast<int64_t>(std::time(nullptr)));
						if (gone)
							ModLoaderLogger::LogInfo(L"[Thumbs] cache tidied: %zu file(s) removed", gone);
					}
					catch (...) {}
				}

				// Asked outside the lock: the installer calls into the UI with its own lock held.
				if (s.config.paused && s.config.paused())
				{
					std::unique_lock<std::mutex> lock(s.mutex);
					s.changed.wait_for(lock, std::chrono::milliseconds(kPausePollMs), [&s] { return s.stopped; });
					continue;
				}

				std::string id;
				ThumbnailInfo info;
				{
					std::lock_guard<std::mutex> lock(s.mutex);
					if (s.stopped || s.queue.empty())
						continue;
					id = std::move(s.queue.front());
					s.queue.pop_front();
					const auto it = s.items.find(id);
					if (it == s.items.end() || it->second.state != ThumbnailState::Loading)
						continue;
					if (s.Now() - it->second.lastWant > kThumbnailStaleMs)
					{
						s.items.erase(it);   // scrolled away; the next Want queues it again
						continue;
					}
					info = it->second.info;
				}

				Outcome out;
				try
				{
					out = Fetch(s, id, info);
				}
				catch (...)
				{
					out = Outcome();
				}

				std::lock_guard<std::mutex> lock(s.mutex);
				const auto it = s.items.find(id);
				if (s.stopped || it == s.items.end() || it->second.info.sha256 != info.sha256)
					continue;
				it->second.state = out.state;
				it->second.bytes = std::move(out.bytes);
			}
		}
	}

	size_t TidyThumbnailCache(const std::wstring& dir, const std::set<std::string>& referencedHashes,
		int64_t nowSec, size_t maxBytes, int64_t maxAgeSec)
	{
		std::vector<CacheFile> files;
		WIN32_FIND_DATAW data;
		const HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
		if (find == INVALID_HANDLE_VALUE)
			return 0;
		do
		{
			if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
				continue;
			CacheFile f;
			for (const wchar_t* c = data.cFileName; *c; ++c)
				f.name += *c < 0x80 ? static_cast<char>(*c) : '?';
			f.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			f.modifiedSec = UnixSeconds(data.ftLastWriteTime);
			files.push_back(std::move(f));
		} while (FindNextFileW(find, &data));
		FindClose(find);

		size_t gone = 0;
		for (const std::string& name : SelectCacheDeletions(files, referencedHashes, nowSec, maxBytes, maxAgeSec))
			if (DeleteFileW((dir + L"\\" + std::wstring(name.begin(), name.end())).c_str()))
				++gone;
		return gone;
	}

	ThumbnailFetcher::ThumbnailFetcher(ThumbnailFetcherConfig config)
		: m_shared(std::make_shared<Shared>())
	{
		m_shared->budget = ThumbnailBudget(config.maxDownloads, config.maxSessionBytes);
		m_shared->config = std::move(config);
	}

	ThumbnailFetcher::~ThumbnailFetcher()
	{
		Shutdown(0);
	}

	void ThumbnailFetcher::SetList(const std::set<std::string>& hashes, bool listSigned)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		m_shared->known = hashes;
		m_shared->listSigned = listSigned;
	}

	void ThumbnailFetcher::Want(const std::string& entryId, const ThumbnailInfo& info)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		Shared& s = *m_shared;
		if (s.stopped || !s.listSigned || !IsFetchable(info))
			return;

		const uint64_t now = s.Now();
		const auto it = s.items.find(entryId);
		if (it != s.items.end())
		{
			if (it->second.info.sha256 == info.sha256 && it->second.info.url == info.url)
			{
				it->second.lastWant = now;
				return;
			}
			s.items.erase(it);   // the list now has another picture for this id
		}

		Item item;
		item.info = info;
		item.lastWant = now;
		s.items.emplace(entryId, std::move(item));
		s.queue.push_back(entryId);

		if (s.running)
		{
			s.changed.notify_all();
			return;
		}
		s.running = true;
		try
		{
			std::thread(&Work, m_shared).detach();
		}
		catch (const std::exception&)
		{
			s.running = false;
			s.items[entryId].state = ThumbnailState::Failed;
			s.queue.pop_back();
		}
	}

	ThumbnailState ThumbnailFetcher::State(const std::string& entryId) const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		const auto it = m_shared->items.find(entryId);
		return it == m_shared->items.end() ? ThumbnailState::None : it->second.state;
	}

	std::string ThumbnailFetcher::TakeBytes(const std::string& entryId)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		const auto it = m_shared->items.find(entryId);
		if (it == m_shared->items.end() || it->second.state != ThumbnailState::Bytes)
			return {};
		it->second.state = ThumbnailState::Handed;
		std::string out = std::move(it->second.bytes);
		std::string().swap(it->second.bytes);
		return out;
	}

	void ThumbnailFetcher::Forget(const std::string& entryId)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		const auto it = m_shared->items.find(entryId);
		if (it != m_shared->items.end() && (it->second.state == ThumbnailState::Bytes || it->second.state == ThumbnailState::Handed))
			m_shared->items.erase(it);
	}

	void ThumbnailFetcher::ClearFailures()
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		for (auto it = m_shared->items.begin(); it != m_shared->items.end();)
			if (it->second.state == ThumbnailState::Failed || it->second.state == ThumbnailState::Mismatch)
				it = m_shared->items.erase(it);
			else
				++it;
	}

	bool ThumbnailFetcher::Shutdown(unsigned waitMs)
	{
		m_shared->cancel = true;
		std::unique_lock<std::mutex> lock(m_shared->mutex);
		m_shared->stopped = true;
		m_shared->queue.clear();
		m_shared->items.clear();
		m_shared->changed.notify_all();
		if (waitMs)
			m_shared->changed.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return !m_shared->running; });
		return !m_shared->running;
	}

	size_t ThumbnailFetcher::Downloads() const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		return m_shared->budget.Downloads();
	}

	size_t ThumbnailFetcher::BytesHeld() const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		size_t total = 0;
		for (const auto& entry : m_shared->items)
			total += entry.second.bytes.size();
		return total;
	}
}
