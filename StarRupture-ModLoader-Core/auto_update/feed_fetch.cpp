#include "feed_fetch.h"
#include "http_get.h"
#include "logging/logger.h"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	using VersionMap::Exe;
	using VersionMap::MapData;

	bool SameExe(uint32_t cl, uint32_t stamp, uint32_t size, const Exe& e)
	{
		return cl == e.cl && stamp == e.imageStamp && size == e.imageSize;
	}

	bool HoldsExe(const MapData& map, const Exe& running)
	{
		for (const auto& b : map.builds)
			if (SameExe(b.gameCL, b.imageStamp, b.imageSize, running))
				return true;
		return false;
	}

	// An `absent` row on the running exe that a request can bind this launch:
	// a global key or a plugin's site key (the plugin may ask for it), or a
	// ModLoader/ key under the running loader's own fingerprint.
	bool HasBindingAbsent(const MapData& map, const Exe& running, const VersionFeed::Coverage& cov)
	{
		for (const auto& row : map.functions)
		{
			if (!row.absent || !SameExe(row.gameCL, row.imageStamp, row.imageSize, running))
				continue;
			if (row.key.compare(0, 10, "ModLoader/") != 0)
				return true;
			for (const auto& need : cov.functions)
				if (need.key == row.key && need.fp == row.fp)
					return true;
		}
		return false;
	}

	// Marks the cache as checked now, so "over a day old" counts from the
	// last request and not from the last change. With no cache file it makes
	// an empty one, which reads as no feed, so a request that got none still
	// waits a day.
	void TouchCache(const std::wstring& path)
	{
		HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		SetFileTime(h, nullptr, nullptr, &now);
		CloseHandle(h);
	}
}

namespace FeedFetch
{
	const char* WhyNot(const Config& cfg)
	{
		if (!cfg.client)
			return "this is the server build";
		if (!cfg.autoUpdate)
			return "[AutoUpdate] Enabled is off";
		if (cfg.url.empty())
			return "no feed URL is compiled in";
		if (cfg.keys.empty())
			return "no public key is compiled in";
		return nullptr;
	}

	uint64_t CacheAgeSeconds(const std::wstring& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
			return UINT64_MAX;
		FILETIME nowFt;
		GetSystemTimeAsFileTime(&nowFt);
		const auto u64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
		const uint64_t now = u64(nowFt), written = u64(data.ftLastWriteTime);
		return now > written ? (now - written) / 10000000ull : 0;
	}

	bool Stage1Wanted(const MapData& baseline, const VersionFeed::SignedFeed* cache,
		const Exe& running, const VersionFeed::Coverage& cov, uint64_t cacheAgeSeconds)
	{
		if (!HoldsExe(baseline, running) && !(cache && HoldsExe(cache->map, running)))
			return true;

		// Known exe: only an `absent` row the cache did not fill, and only once a day.
		if (cacheAgeSeconds <= kDaySeconds)
			return false;
		VersionFeed::MergeReport report;
		const MapData merged = VersionFeed::Merge(baseline, cache ? cache->map : MapData{}, running, report);
		return HasBindingAbsent(merged, running, cov);
	}

	Result Refresh(const Config& cfg, const char* why)
	{
		if (const char* reason = WhyNot(cfg))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (%hs): %hs", why, reason);
			return Result::NotAllowed;
		}

		ModLoaderLogger::LogInfo(L"[VersionFeed] Fetching the feed (%hs)", why);
		HttpGetOptions options;
		options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = kOperationMs;
		options.deadlineMs = kDeadlineMs;
		options.maxBytes = VersionFeed::kMaxFileBytes;
		options.allowPlainHttp = cfg.allowPlainHttp;
		const std::string file = HttpGet(cfg.url.c_str(), "version map feed", options);
		TouchCache(cfg.cachePath);
		if (file.empty())
		{
			ModLoaderLogger::LogWarn(L"[VersionFeed] The feed could not be fetched; the cache is as it was");
			return Result::Failed;
		}

		VersionFeed::SignedFeed feed;
		std::string error;
		if (!VersionFeed::ReadSigned(file, cfg.keys, feed, error))
		{
			ModLoaderLogger::LogWarn(L"[VersionFeed] The fetched feed is refused (%hs); the cache is as it was", error.c_str());
			return Result::Failed;
		}
		if (!VersionFeed::StoreCache(cfg.cachePath, file, cfg.keys, error))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] The fetched feed is not stored (%hs)", error.c_str());
			return Result::Unchanged;
		}
		ModLoaderLogger::LogInfo(L"[VersionFeed] Feed seq %llu stored for the next launch", static_cast<unsigned long long>(feed.seq));
		return Result::Stored;
	}

	bool Stage1(const Config& cfg, bool mainParked, void (*announce)(uint32_t cl))
	{
		if (const char* reason = WhyNot(cfg))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (stage 1): %hs", reason);
			return false;
		}

		VersionFeed::SignedFeed cache;
		const bool haveCache = VersionFeed::ReadCache(cfg.cachePath, cfg.keys, cache);
		if (!Stage1Wanted(VersionMap::BaselineMap(), haveCache ? &cache : nullptr, VersionMap::RunningExe(),
			VersionFeed::LoaderCoverage(), CacheAgeSeconds(cfg.cachePath)))
			return false;
		if (!mainParked)
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (stage 1): the main thread is not parked, so WinHTTP is not safe yet");
			return false;
		}
		if (announce)
			announce(VersionMap::RunningExe().cl);
		Refresh(cfg, "stage 1");
		return true;
	}

	void AfterRefusal(const Config& cfg, bool stage1Asked)
	{
		if (stage1Asked)
			return;
		Refresh(cfg, "after a refusal, for the next launch");
	}

	void Stage3(const Config& cfg)
	{
		if (const char* reason = WhyNot(cfg))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (stage 3): %hs", reason);
			return;
		}
		if (CacheAgeSeconds(cfg.cachePath) <= kDaySeconds)
			return;
		Refresh(cfg, "stage 3");
	}
}
