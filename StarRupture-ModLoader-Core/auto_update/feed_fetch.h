#pragma once

#include "memory_scanner/version_feed.h"
#include "memory_scanner/version_map.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// feed_fetch.h -- when the loader asks for the signed version-map feed, and the
// one request it makes (see version_feed.h for the file).
//
// The loader asks at every start whether a newer feed than the cached one is
// out. The request is conditional: it carries the ETag the last answer had, so
// when nothing changed the answer is a 304 with no body. A newer feed is
// verified, stored, and read by the same start (Stage 1); the map in memory is
// built once, right after, and never changes after it.
//
//   Stage 1       every start, on the parked-main-thread path only: the check,
//                 then the cache is read for this launch.
//   AfterRefusal  a gate refusal that did not fetch this launch: fetch for the
//                 next one, before Quit's exit.
//   Stage3        on the auto-update thread, only when Stage 1 could not ask
//                 (the injected path): fetch for the next launch.
//
// The client build fetches version-map-client.v1.txt (VMAP_FEED_URL) and the
// dedicated server build version-map-server.v1.txt (VMAP_SERVER_FEED_URL), each
// into its own cache file, and each refuses the other's file. Nothing is
// fetched with `[AutoUpdate] Enabled` off, with no URL compiled in for the
// build or with no public key. A request that fails (offline, a 404, a
// timeout, a bad signature, the other build's file) leaves the cache as it was.
//
// On the dedicated server the wait for the request ends after kDeadlineMs,
// whatever WinHTTP is doing (RunWithin). An answer that comes later is dropped.
// ---------------------------------------------------------------------------

namespace FeedFetch
{
	// Per-operation timeout, wall-clock deadline for the request.
	constexpr unsigned kOperationMs = 2000;
	constexpr unsigned kDeadlineMs  = 8000;

	struct Config
	{
		std::string url;                             // empty: no fetch
		std::vector<VersionFeed::PublicKey> keys;    // none: no fetch
		std::wstring cachePath;
		VersionFeed::Flavor flavor = VersionFeed::kBuildFlavor;   // which file: the client's or the server's
		std::string  catalogUrl;                     // plugin-catalog.v1.txt beside the map feed; empty: no catalog fetch (the server build)
		std::wstring catalogCachePath;
		bool autoUpdate  = true;                     // modloader.ini [AutoUpdate] Enabled
		bool allowPlainHttp = false;                 // for tests that run a local server
	};

	// This loader's settings: VMAP_FEED_URL (client) or VMAP_SERVER_FEED_URL
	// (server), the compiled keys, the cache next to the other loader files.
	Config LoaderConfig();

	// Why the loader makes no request under `cfg`, or null when it may.
	const char* WhyNot(const Config& cfg);

	enum class Result
	{
		NotAllowed,   // no request was made
		Failed,       // no usable answer (a 404, a timeout, a bad signature, a file over the cap)
		Unchanged,    // nothing newer: a 304, or a verified feed whose seq is not above the cache's
		Stored,       // verified, newer, and now the cache
	};

	// Runs `request` on a thread of its own and waits at most limitMs for it.
	// False when it has not ended by then, or no thread could be started. A
	// request that is given up on runs on to its own end, so it must own all it
	// touches; its thread holds a reference on this module until then. Not to be
	// called under the loader lock.
	bool RunWithin(std::function<void()> request, unsigned limitMs);

	// One conditional GET of cfg.url, verified, then stored if its seq is above
	// the cache's. The ETag of the answer is kept next to the cache, with the
	// seq it belongs to, and sent back only while the cache still holds that
	// seq. `why` is for the log.
	Result Refresh(const Config& cfg, const char* why);

	// One GET of cfg.catalogUrl (the signed plugin catalog), stored in the
	// catalog cache by PluginCatalog::Store. Unchanged: a refused or repeated
	// file; Failed: nothing usable came back. On the auto-update thread, after
	// Stage3; the accessor serves a stored catalog at once. Never throws.
	Result RefreshCatalog(const Config& cfg);

	// Stage 1: the check. `mainParked`: the main thread is parked on the APC
	// path, the only place the request is safe. Returns whether a request was
	// made, so a refusal that follows does not make a second.
	// `announce`, if given, is called with the running CL just before the
	// request when neither the loader's map nor the cache holds that exe.
	bool Stage1(const Config& cfg, bool mainParked, void (*announce)(uint32_t cl) = nullptr);

	// After a gate refusal; does nothing when Stage 1 already asked.
	void AfterRefusal(const Config& cfg, bool stage1Asked);

	// On the auto-update thread: the check for the next launch, when no request
	// went out yet this launch.
	void Stage3(const Config& cfg);
}
