#include "feed_fetch.h"
#include "feed_etag.h"
#include "http_get.h"
#include "logging/logger.h"
#include "plugins/plugin_catalog.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>

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

	// The feed's request and all it writes to: its thread can outlive the wait.
	struct Request
	{
		std::string url, file, etag;
		bool notModified = false;
		std::atomic<bool> cancel{ false };
		HttpGetOptions options;
	};

	struct Job
	{
		std::function<void()> request;
		std::promise<void> ended;
		HMODULE module;
	};

	DWORD WINAPI JobMain(void* argument)
	{
		HMODULE module;
		{
			const std::unique_ptr<Job> job(static_cast<Job*>(argument));
			module = job->module;
			try { job->request(); } catch (...) {}
			job->ended.set_value();
		}
		FreeLibraryAndExitThread(module, 0);
	}
}

namespace FeedFetch
{
	bool RunWithin(std::function<void()> request, unsigned limitMs)
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&JobMain), &module))
			return false;
		auto job = std::make_unique<Job>(Job{ std::move(request), {}, module });
		const std::future<void> ended = job->ended.get_future();
		const HANDLE thread = CreateThread(nullptr, 0, JobMain, job.get(), 0, nullptr);
		if (!thread)
		{
			FreeLibrary(module);
			return false;
		}
		job.release();   // the thread's now
		CloseHandle(thread);
		return ended.wait_for(std::chrono::milliseconds(limitMs)) == std::future_status::ready;
	}

	const char* WhyNot(const Config& cfg)
	{
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
		const bool haveCache = VersionFeed::ReadCache(cfg.cachePath, cfg.keys, cache, cfg.flavor);
		const auto request = std::make_shared<Request>();
		request->url = cfg.url;
		HttpGetOptions& options = request->options;
		options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = kOperationMs;
		options.deadlineMs = kDeadlineMs;
		options.maxBytes = VersionFeed::kMaxFileBytes;
		options.allowPlainHttp = cfg.allowPlainHttp;
		options.ifNoneMatch = haveCache ? ReadEtag(cfg.cachePath, cache.seq) : std::string();
		options.etag = &request->etag;
		options.notModified = &request->notModified;

		g_requestedThisLaunch = true;
		ModLoaderLogger::LogInfo(L"[VersionFeed] Checking for a newer version object map (%hs)", why);
		const auto get = [request] { request->file = HttpGet(request->url.c_str(), "version map feed", request->options); };
		if (cfg.flavor == VersionFeed::Flavor::Server)
		{
			// HttpGet looks at its deadline between steps only, and redirects and
			// connect retries run inside one step, so the wait is bounded here.
			options.cancel = &request->cancel;
			if (!RunWithin(get, kDeadlineMs))
			{
				request->cancel = true;
				ModLoaderLogger::LogInfo(L"[VersionFeed] The version object map request did not end within %u ms or could not be started, and is given up on; the cache is as it was", kDeadlineMs);
				return Result::Failed;
			}
		}
		else
			get();
		const std::string& file = request->file;
		const std::string& etag = request->etag;
		const bool notModified = request->notModified;
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
		if (!VersionFeed::ReadSigned(file, cfg.keys, feed, error, cfg.flavor))
		{
			ModLoaderLogger::LogWarn(L"[VersionFeed] The fetched version object map is refused (%hs); the cache is as it was", error.c_str());
			return Result::Failed;
		}
		if (!VersionFeed::StoreCache(cfg.cachePath, file, cfg.keys, error, cfg.flavor))
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

	Result RefreshCatalog(const Config& cfg)
	{
		const char* reason = !cfg.autoUpdate ? "[AutoUpdate] Enabled is off" : cfg.catalogUrl.empty() ? "no catalog URL is compiled in"
			: cfg.keys.empty() ? "no public key is compiled in" : nullptr;
		if (reason)
		{
			ModLoaderLogger::LogInfo(L"[PluginCatalog] No catalog request: %hs", reason);
			return Result::NotAllowed;
		}

		try
		{
			HttpGetOptions options;
			options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = kOperationMs;
			options.deadlineMs = kDeadlineMs;
			options.maxBytes = VersionFeed::kMaxFileBytes;
			options.allowPlainHttp = cfg.allowPlainHttp;

			const std::string file = HttpGet(cfg.catalogUrl.c_str(), "plugin catalog", options);
			if (file.empty())
			{
				ModLoaderLogger::LogInfo(L"[PluginCatalog] The plugin catalog could not be fetched; the cache is as it was");
				return Result::Failed;
			}
			std::string error;
			switch (PluginCatalog::Store(cfg.catalogCachePath, file, cfg.keys, error))
			{
			case PluginCatalog::Outcome::Stored:
				ModLoaderLogger::LogInfo(L"[PluginCatalog] Plugin catalog stored");
				return Result::Stored;
			case PluginCatalog::Outcome::Unchanged:
				ModLoaderLogger::LogInfo(L"[PluginCatalog] No newer plugin catalog");
				return Result::Unchanged;
			case PluginCatalog::Outcome::RefusedRollback:
			case PluginCatalog::Outcome::RefusedConflict:
				ModLoaderLogger::LogWarn(L"[PluginCatalog] The fetched plugin catalog is refused (%hs); the cache is as it was", error.c_str());
				return Result::Unchanged;
			case PluginCatalog::Outcome::Rejected:
				break;
			}
			ModLoaderLogger::LogWarn(L"[PluginCatalog] The fetched plugin catalog is rejected (%hs); the cache is as it was", error.c_str());
		}
		catch (...)
		{
			ModLoaderLogger::LogWarn(L"[PluginCatalog] The plugin catalog fetch failed unexpectedly; the cache is as it was");
		}
		return Result::Failed;
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
			const bool haveCache = VersionFeed::ReadCache(cfg.cachePath, cfg.keys, cache, cfg.flavor);
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
