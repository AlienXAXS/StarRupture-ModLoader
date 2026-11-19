#include "feed_fetch.h"
#include "http_get.h"
#include "logging/logger.h"

#include <atomic>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	using VersionMap::Exe;
	using VersionMap::MapData;

	// Set once a request goes out, so Stage 3 asks only when Stage 1 could not.
	std::atomic<bool> g_requestedThisLaunch{ false };

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

	// The ETag of the answer the cache holds, kept next to it as "<seq> <etag>".
	std::wstring EtagPath(const std::wstring& cachePath)
	{
		return cachePath + L".etag";
	}

	// The stored ETag, only when it belongs to the cache's seq and is plain text.
	std::string ReadEtag(const std::wstring& cachePath, uint64_t cacheSeq)
	{
		std::string text;
		HANDLE h = CreateFileW(EtagPath(cachePath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return {};
		char buf[256];
		DWORD got = 0;
		if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr))
			text.assign(buf, got);
		CloseHandle(h);

		char* end = nullptr;
		const unsigned long long seq = strtoull(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != ' ' || seq != cacheSeq)
			return {};
		std::string etag(end + 1);
		for (char c : etag)
			if (c < 0x20 || c > 0x7E)
				return {};
		return etag;
	}

	// Best effort: with no ETag on file the next start asks for the whole feed.
	void WriteEtag(const std::wstring& cachePath, uint64_t seq, const std::string& etag)
	{
		HANDLE h = CreateFileW(EtagPath(cachePath).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		const std::string text = std::to_string(seq) + " " + etag;
		DWORD wrote = 0;
		WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
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

	Result Refresh(const Config& cfg, const char* why)
	{
		if (const char* reason = WhyNot(cfg))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (%hs): %hs", why, reason);
			return Result::NotAllowed;
		}

		// Ask only whether anything is newer than the cache: with the ETag of
		// the answer the cache came from, an unchanged feed comes back as a 304.
		VersionFeed::SignedFeed cache;
		const bool haveCache = VersionFeed::ReadCache(cfg.cachePath, cfg.keys, cache);
		std::string etag;
		bool notModified = false;
		HttpGetOptions options;
		options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = kOperationMs;
		options.deadlineMs = kDeadlineMs;
		options.maxBytes = VersionFeed::kMaxFileBytes;
		options.allowPlainHttp = cfg.allowPlainHttp;
		options.ifNoneMatch = haveCache ? ReadEtag(cfg.cachePath, cache.seq) : std::string();
		options.etag = &etag;
		options.notModified = &notModified;

		g_requestedThisLaunch = true;
		ModLoaderLogger::LogInfo(L"[VersionFeed] Checking for a newer version object map (%hs)", why);
		const std::string file = HttpGet(cfg.url.c_str(), "version map feed", options);
		if (notModified)
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No newer version object map: seq %llu is current", static_cast<unsigned long long>(cache.seq));
			return Result::Unchanged;
		}
		if (file.empty())
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] The version object map could not be fetched; the cache is as it was");
			return Result::Failed;
		}

		VersionFeed::SignedFeed feed;
		std::string error;
		if (!VersionFeed::ReadSigned(file, cfg.keys, feed, error))
		{
			ModLoaderLogger::LogWarn(L"[VersionFeed] The fetched version object map is refused (%hs); the cache is as it was", error.c_str());
			return Result::Failed;
		}
		if (!VersionFeed::StoreCache(cfg.cachePath, file, cfg.keys, error))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] The fetched version object map is not stored (%hs)", error.c_str());
			// The cache's own feed again: remember its ETag, so the next start can ask for a 304.
			if (haveCache && feed.seq == cache.seq && !etag.empty())
				WriteEtag(cfg.cachePath, feed.seq, etag);
			return Result::Unchanged;
		}
		if (!etag.empty())
			WriteEtag(cfg.cachePath, feed.seq, etag);
		ModLoaderLogger::LogInfo(L"[VersionFeed] Version object map seq %llu stored", static_cast<unsigned long long>(feed.seq));
		return Result::Stored;
	}

	bool Stage1(const Config& cfg, bool mainParked, void (*announce)(uint32_t cl))
	{
		if (const char* reason = WhyNot(cfg))
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (stage 1): %hs", reason);
			return false;
		}
		if (!mainParked)
		{
			ModLoaderLogger::LogInfo(L"[VersionFeed] No feed request (stage 1): the main thread is not parked, so WinHTTP is not safe yet");
			return false;
		}

		// The splash says so only for an exe nothing holds yet: that start waits on the answer.
		if (announce)
		{
			const Exe running = VersionMap::RunningExe();
			VersionFeed::SignedFeed cache;
			const bool haveCache = VersionFeed::ReadCache(cfg.cachePath, cfg.keys, cache);
			if (!HoldsExe(VersionMap::BaselineMap(), running) && !(haveCache && HoldsExe(cache.map, running)))
				announce(running.cl);
		}

		if (Refresh(cfg, "stage 1") == Result::Stored)
			ModLoaderLogger::LogInfo(L"[VersionFeed] Newer rows for the version object map were fetched and are read on this start");
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
		if (g_requestedThisLaunch)
			return;
		Refresh(cfg, "stage 3, for the next launch");
	}
}
