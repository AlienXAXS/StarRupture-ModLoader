#pragma once

#include "memory_scanner/version_feed.h"
#include "memory_scanner/version_map.h"

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// feed_fetch.h -- when the loader asks for the signed version-map feed, and the
// one request it makes (see version_feed.h for the file).
//
// The fetch only ever fills the cache file for the next launch, except at
// Stage 1, which fetches before the cache is read for this one. The map in
// memory is built once, at Stage 1, and never changes after it.
//
//   Stage 1       the running exe is in neither the loader's map nor the cache,
//                 or has an `absent` row that can bind this launch and the
//                 cache is over a day old; on the parked-main-thread path only.
//   AfterRefusal  a gate refusal that did not fetch this launch: fetch for the
//                 next one, before Quit's exit.
//   Stage3        on the auto-update thread, at most once a day.
//
// Nothing is fetched on the server build, with `[AutoUpdate] Enabled` off, with
// no VMAP_FEED_URL compiled in or with no public key.
// ---------------------------------------------------------------------------

namespace FeedFetch
{
	// Per-operation timeout, wall-clock deadline for the request.
	constexpr unsigned kOperationMs = 2000;
	constexpr unsigned kDeadlineMs  = 8000;

	// How long a cache counts as fresh.
	constexpr uint64_t kDaySeconds = 24 * 60 * 60;

	struct Config
	{
		std::string url;                             // empty: no fetch
		std::vector<VersionFeed::PublicKey> keys;    // none: no fetch
		std::wstring cachePath;
		bool client      = false;                    // the server never fetches
		bool autoUpdate  = true;                     // modloader.ini [AutoUpdate] Enabled
		bool allowPlainHttp = false;                 // for tests that run a local server
	};

	// This loader's settings: VMAP_FEED_URL, the compiled keys, the cache next
	// to the other loader files.
	Config LoaderConfig();

	// Why the loader makes no request under `cfg`, or null when it may.
	const char* WhyNot(const Config& cfg);

	// Seconds since the cache file was last written (a refresh that finds
	// nothing newer counts), or UINT64_MAX with no cache file.
	uint64_t CacheAgeSeconds(const std::wstring& path);

	// Stage 1's trigger. `cache` is the verified feed in the cache, if any.
	bool Stage1Wanted(const VersionMap::MapData& baseline, const VersionFeed::SignedFeed* cache,
		const VersionMap::Exe& running, const VersionFeed::Coverage& cov, uint64_t cacheAgeSeconds);

	enum class Result
	{
		NotAllowed,   // no request was made
		Failed,       // no usable answer (a 404, a timeout, a bad signature, a file over the cap)
		Unchanged,    // verified, but not newer than the cache
		Stored,       // verified and now the cache
	};

	// One GET of cfg.url, verified, then stored if newer. `why` is for the log.
	Result Refresh(const Config& cfg, const char* why);

	// Stage 1: fetch if wanted. `mainParked`: the main thread is parked on the
	// APC path, the only place the request is safe. Returns whether a request
	// was made, so a refusal that follows does not make a second.
	// `announce`, if given, is called with the running CL just before the request.
	bool Stage1(const Config& cfg, bool mainParked, void (*announce)(uint32_t cl) = nullptr);

	// After a gate refusal; does nothing when Stage 1 already asked.
	void AfterRefusal(const Config& cfg, bool stage1Asked);

	// On the auto-update thread, at most once a day.
	void Stage3(const Config& cfg);
}
