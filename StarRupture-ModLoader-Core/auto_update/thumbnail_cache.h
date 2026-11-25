#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// thumbnail_cache.h -- the rules of the Browse thumbnails' disk cache and
// download budget, as pure functions, so a test needs no loader and no disk.
// thumbnail_fetcher.h does the I/O around them.
//
// A cached picture is ModLoader\thumbs\<64 lowercase hex>.png, named by the
// SHA-256 of its bytes, so the name can never be built from anything the index
// says except a checked hash. While it is written it is <hash>.png.tmp.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	constexpr size_t   kMaxThumbnailDownloads    = 40;                 // per session; a failed one counts
	constexpr size_t   kMaxThumbnailSessionBytes = 16 * 1024 * 1024;   // per session, declared sizes of the downloads started
	constexpr unsigned kThumbnailDeadlineMs      = 15000;              // one picture, every step
	constexpr size_t   kMaxThumbnailCacheBytes   = 32 * 1024 * 1024;   // ModLoader\thumbs, checked once per session
	constexpr int64_t  kThumbnailOrphanAgeSec    = 30LL * 24 * 3600;   // an unlisted picture is kept this long

	// 64 lowercase hex.
	inline bool IsThumbnailHash(const std::string& s)
	{
		if (s.size() != 64)
			return false;
		for (const char c : s)
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				return false;
		return true;
	}

	// The cache file's name for a picture; empty when `sha256` is not a hash.
	inline std::string ThumbnailCacheName(const std::string& sha256)
	{
		return IsThumbnailHash(sha256) ? sha256 + ".png" : std::string();
	}

	// "<hash>.png"
	inline bool IsThumbnailCacheName(const std::string& name)
	{
		return name.size() == 68 && name.compare(64, 4, ".png") == 0 && IsThumbnailHash(name.substr(0, 64));
	}

	// "<hash>.png.tmp", what an interrupted write leaves behind.
	inline bool IsThumbnailTempName(const std::string& name)
	{
		return name.size() == 72 && name.compare(68, 4, ".tmp") == 0 && IsThumbnailCacheName(name.substr(0, 68));
	}

	// dir + "\" + the cache name; empty when `sha256` is not a hash.
	inline std::wstring ThumbnailCachePath(const std::wstring& dir, const std::string& sha256)
	{
		const std::string name = ThumbnailCacheName(sha256);
		return name.empty() ? std::wstring() : dir + L"\\" + std::wstring(name.begin(), name.end());
	}

	// What a session may still download.
	class ThumbnailBudget
	{
	public:
		ThumbnailBudget(size_t maxDownloads = kMaxThumbnailDownloads, size_t maxBytes = kMaxThumbnailSessionBytes)
			: m_maxDownloads(maxDownloads), m_maxBytes(maxBytes) {}

		// Starts one download of at most `declared` bytes, if both limits allow it.
		// A download that later fails stays counted.
		bool Reserve(size_t declared)
		{
			if (m_downloads >= m_maxDownloads || declared > m_maxBytes - m_bytes)
				return false;
			++m_downloads;
			m_bytes += declared;
			return true;
		}

		size_t Downloads() const { return m_downloads; }
		size_t Bytes() const { return m_bytes; }

	private:
		size_t m_maxDownloads;
		size_t m_maxBytes;
		size_t m_downloads = 0;
		size_t m_bytes = 0;
	};

	struct CacheFile
	{
		std::string name;
		uint64_t    size = 0;
		int64_t     modifiedSec = 0;   // seconds since 1970
	};

	// Which files in the cache folder to delete, by name. Only two kinds of name
	// are ever chosen: a leftover <hash>.png.tmp, and a <hash>.png. A picture the
	// current list names is kept. One it does not name goes when it is older than
	// maxAgeSec, and then, while the pictures left total more than maxBytes, the
	// oldest unnamed ones. Any other name is not looked at.
	inline std::vector<std::string> SelectCacheDeletions(const std::vector<CacheFile>& files,
		const std::set<std::string>& referencedHashes, int64_t nowSec, size_t maxBytes, int64_t maxAgeSec)
	{
		std::vector<std::string> doomed;
		std::vector<const CacheFile*> unlisted;   // kept so far, not named by the list
		uint64_t total = 0;
		for (const CacheFile& f : files)
		{
			if (IsThumbnailTempName(f.name))
			{
				doomed.push_back(f.name);
				continue;
			}
			if (!IsThumbnailCacheName(f.name))
				continue;
			const bool listed = referencedHashes.count(f.name.substr(0, 64)) != 0;
			if (!listed && nowSec - f.modifiedSec > maxAgeSec)
			{
				doomed.push_back(f.name);
				continue;
			}
			total += f.size;
			if (!listed)
				unlisted.push_back(&f);
		}
		std::sort(unlisted.begin(), unlisted.end(), [](const CacheFile* a, const CacheFile* b)
		{
			return a->modifiedSec != b->modifiedSec ? a->modifiedSec < b->modifiedSec : a->name < b->name;
		});
		for (const CacheFile* f : unlisted)
		{
			if (total <= maxBytes)
				break;
			doomed.push_back(f->name);
			total -= f->size;
		}
		return doomed;
	}
}
