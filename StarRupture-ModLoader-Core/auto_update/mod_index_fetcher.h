#pragma once

#include "auto_update/mod_index.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// mod_index_fetcher.h -- fetches the mod index (mod_index.h) once, on a worker
// thread, and holds the answer in memory for the Browse tab to draw.
//
// The tab, once per frame:
//
//   auto snap = ModIndex::Instance().Get();       // no I/O, no allocation
//   switch (snap->state) {
//   case State::Idle:    ModIndex::Instance().RequestFirstLoad();  // once is enough, any frame
//   case State::Loading: ...draw "Loading..."
//   case State::Failed:  ...draw snap->error and a Retry button -> Instance().Retry()
//   case State::Ready:   ...draw snap->entries; with none, a Retry button too
//   }
//
// Threads: RequestFirstLoad, Retry and Get may be called from any thread, as
// often as wanted (the render thread included): they take a short lock and
// never wait on the network. The request runs on one worker thread, started
// by the first RequestFirstLoad or by a Retry, never by Get. A snapshot is
// never changed after it is published, so a shared_ptr the tab holds stays
// valid and consistent while the worker publishes the next one.
//
// Shutdown: the worker owns a share of the state, so it can finish after the
// Fetcher is gone and at process exit. Shutdown(waitMs) stops new requests and
// waits up to waitMs for a running one (the request itself ends by the HttpGet
// deadline). This class does no disk I/O itself; the loader's own instance gets its
// fetch function from mod_index_signed.h, which keeps the cache file.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	enum class State { Idle, Loading, Ready, Failed };

	struct Snapshot
	{
		State state = State::Idle;
		std::vector<Entry> entries;     // Ready only
		int  skipped   = 0;             // objects the parser dropped
		bool truncated = false;         // the list was cut at kMaxEntries
		std::string error;              // Failed only: plain words for the player
	};

	// Where the index text comes from: the index (JSON) on success, or an empty
	// string on any failure, with `error` set to plain words for the player (left
	// empty, the Fetcher says the download failed). mod_index_signed.h makes the
	// loader's own, which downloads, verifies and caches.
	using FetchFn = std::function<std::string(const std::string& url, std::string& error)>;

	class Fetcher
	{
	public:
		Fetcher(std::string url, FetchFn fetch);
		~Fetcher();
		Fetcher(const Fetcher&) = delete;
		Fetcher& operator=(const Fetcher&) = delete;

		// Idle -> Loading, and the worker starts. Any other state: nothing.
		void RequestFirstLoad();

		// Failed, or Ready with no entries -> Loading, and a new request starts.
		// Any other state: nothing.
		void Retry();

		// The current snapshot. Never null.
		std::shared_ptr<const Snapshot> Get() const;

		// No new requests after this; waits up to waitMs for a running one.
		// Returns whether no request is still running.
		bool Shutdown(unsigned waitMs);

		struct Shared;   // the state the worker shares; defined in the .cpp

	private:
		void Start(bool retry);
		std::shared_ptr<Shared> m_shared;
	};

	// The loader's one index: the URL from modloader.ini [AutoUpdate] ModIndexUrl,
	// else the signed mod-index.v1.txt beside the loader's version map feed. Made on first use and kept to the end of the process.
	Fetcher& Instance();
}
