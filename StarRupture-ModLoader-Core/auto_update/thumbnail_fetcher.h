#pragma once

#include "auto_update/mod_index.h"
#include "auto_update/mod_install.h"
#include "auto_update/readme_textures.h"
#include "auto_update/thumbnail_cache.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>

// ---------------------------------------------------------------------------
// thumbnail_fetcher.h -- gets the pictures of the Browse cards (mod_index.h,
// ThumbnailInfo) onto the loader's memory, checked, on one worker thread. It
// makes no texture: ThumbnailTextures (thumbnail_textures.h) takes the bytes.
//
// A picture is fetched only for an entry of a list that passed its signature
// (SetList): the hash that guards the bytes comes from that list. For a list the
// player supplied unsigned, Want does nothing and the card keeps its placeholder.
//
// What the worker does for one picture, in order:
//   1. ModLoader\thumbs\<sha256>.png, if it is there: read, size and SHA-256 and
//      the PNG header checked again. Good: used, no network. Bad: deleted.
//   2. The session budget (thumbnail_cache.h): at most 40 downloads and 16 MiB.
//   3. One HTTPS GET of the entry's address with the installer's HttpHop: no
//      redirect is followed (raw.githubusercontent.com answers 200 itself, and
//      ModInstall::IsAllowedRedirect allows none from it), at most `size` bytes,
//      15 s for the whole request.
//   4. Everything in memory, before any write: body length equals `size`, SHA-256
//      equals the list's, the PNG header is a PNG within kMaxThumbnailWidth x
//      kMaxThumbnailHeight (thumbnail_check.h).
//   5. Written as <sha256>.png.tmp, then renamed to <sha256>.png, never over a
//      file that is there. A write that fails costs only the cache: the picture
//      is still shown this session.
// Any failure leaves nothing on disk, logs one plain line (host and reason, never
// the query) and marks the picture Failed (or Mismatch for a wrong size or hash).
// The player sees the placeholder either way; a failed picture is tried again
// only after ClearFailures (the Retry button) or a restart.
//
// One at a time, in the order the cards asked, and never while an install runs
// (`paused`). A card that has not asked again for kStaleMs when its turn comes is
// skipped: it was scrolled away.
//
// Once per session, before the first fetch of a signed list, the folder is
// tidied (thumbnail_cache.h SelectCacheDeletions): leftover .tmp files, pictures
// the list does not name that are over 30 days old, and the oldest of those
// above 32 MiB. No other file is touched.
//
// Threads: Want, State, SetList, ClearFailures, Forget and TakeBytes take a short
// lock and do no I/O, no hashing and no network, so the render thread may call
// them every frame. Network, hashing and disk are the worker's alone. The worker
// never touches a texture or ImGui, and it owns a share of the state, so it may
// finish after the fetcher is gone. Shutdown(waitMs) cancels the request in
// flight, drops everything held and waits up to waitMs for the worker.
//
// ThumbnailTextures, in the same header, is the other half: the pictures as GPU
// textures, on the game thread. See the comment above it.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	enum class ThumbnailState
	{
		None,       // not asked for, or not allowed (unsigned list, no picture)
		Loading,    // queued or being fetched
		Bytes,      // verified bytes are in memory, waiting for TakeBytes
		Handed,     // TakeBytes gave them out; the texture side has them
		Failed,     // could not download, not a PNG, too big, over the budget
		Mismatch,   // the size or the SHA-256 differs from the list's
	};

	constexpr uint64_t kThumbnailStaleMs = 2000;

	struct ThumbnailFetcherConfig
	{
		std::wstring cacheDir;                      // ...\ModLoader\thumbs, no trailing backslash; created when needed
		ModInstall::HopFn hop;                      // ModInstall::HttpHop() in the loader
		std::function<bool()> paused;               // true while an install runs; null: never
		std::function<uint64_t()> nowMs;            // null: the steady clock
		size_t maxDownloads    = kMaxThumbnailDownloads;
		size_t maxSessionBytes = kMaxThumbnailSessionBytes;
		unsigned deadlineMs    = kThumbnailDeadlineMs;
	};

	class ThumbnailFetcher
	{
	public:
		explicit ThumbnailFetcher(ThumbnailFetcherConfig config);
		~ThumbnailFetcher();
		ThumbnailFetcher(const ThumbnailFetcher&) = delete;
		ThumbnailFetcher& operator=(const ThumbnailFetcher&) = delete;

		// The hashes the current list names (what the cache tidy keeps), and
		// whether the list passed its signature. Render thread, when the list
		// changes. Until a signed list is set, no picture is fetched.
		void SetList(const std::set<std::string>& hashes, bool listSigned);

		// A card wants its picture. Queues it the first time; afterwards only
		// keeps it from going stale. A picture the entry does not have, or an
		// unsigned list, is ignored. Render thread, per visible card per frame.
		void Want(const std::string& entryId, const ModIndex::ThumbnailInfo& info);

		ThumbnailState State(const std::string& entryId) const;

		// The verified bytes, handed out once; empty unless the state is Bytes.
		// Game thread, as the decode that follows it.
		std::string TakeBytes(const std::string& entryId);

		// The texture was freed: forgets the picture, so the next Want fetches it
		// again (from the cache folder, with no download). Only for Bytes/Handed.
		void Forget(const std::string& entryId);

		// Failed and Mismatch pictures may be asked for again.
		void ClearFailures();

		// No new work; cancels a request in flight and drops what is held. Waits
		// up to waitMs for the worker. Returns whether it has ended. May be called twice.
		bool Shutdown(unsigned waitMs);

		// For tests and the log.
		size_t Downloads() const;
		size_t BytesHeld() const;

		struct Shared;   // the state the worker shares; defined in the .cpp

	private:
		std::shared_ptr<Shared> m_shared;
	};

	// Deletes what SelectCacheDeletions picks in `dir`; returns how many files
	// went. Used by the worker, public for the test.
	size_t TidyThumbnailCache(const std::wstring& dir, const std::set<std::string>& referencedHashes,
		int64_t nowSec, size_t maxBytes = kMaxThumbnailCacheBytes, int64_t maxAgeSec = kThumbnailOrphanAgeSec);

	// ------------------------------------------------------------------------
	// ThumbnailTextures -- the Browse pictures as GPU textures, by entry id. Pure
	// logic over TextureOps (readme_textures.h, the same one the README view
	// uses), so no ImGui and no game are needed to test it. `ops.take` is not
	// used: the bytes come from `hooks.take`, by id.
	//
	//   render thread  Request(id) once per frame for a card whose fetcher state
	//                  is Bytes; Get(id) to draw. Neither decodes nor loads.
	//   game thread    the posted job: takes the bytes, loads the texture (WIC
	//                  decode), checks its size, publishes it. TextureOps::post
	//                  hops there, as for the README pictures.
	// One job at a time; a Request while one is in flight does nothing and the
	// next frame asks again.
	//
	// At most kMaxThumbnailTextures textures are held. A new one frees the least
	// recently drawn (Get) and tells `hooks.released`, so the fetcher forgets that
	// picture and a later Request brings it back from the cache folder. A texture
	// drawn within the last kThumbnailRecentMs is not taken away: with that many
	// cards on screen the extra ones wait, instead of the table loading and
	// freeing the same pictures every frame. Each Load is paired with a FreeTexture
	// on every path; Shutdown frees what is held and waits for a load that is
	// running now.
	// ------------------------------------------------------------------------

	constexpr size_t   kMaxThumbnailTextures = 16;
	constexpr uint64_t kThumbnailRecentMs    = 1000;

	struct ThumbnailTexturesHooks
	{
		// The verified bytes of one picture, once (ThumbnailFetcher::TakeBytes). Game thread.
		std::function<std::string(const std::string& id)> take;
		// A texture was freed to make room (ThumbnailFetcher::Forget). Game thread, no lock held.
		std::function<void(const std::string& id)> released;
		std::function<uint64_t()> nowMs;   // null: the steady clock
	};

	class ThumbnailTextures
	{
	public:
		ThumbnailTextures(TextureOps ops, ThumbnailTexturesHooks hooks);
		~ThumbnailTextures();
		ThumbnailTextures(const ThumbnailTextures&) = delete;
		ThumbnailTextures& operator=(const ThumbnailTextures&) = delete;

		// Asks for the texture to be made, when none is in flight, the id has not
		// been tried and there is room (see above). Render thread.
		void Request(const std::string& id);

		// What to draw; counts as drawn now. state None: not made (yet). Render thread.
		Picture Get(const std::string& id);

		// Textures held (Ready).
		size_t Held() const;

		// No more jobs; frees what is held and waits up to waitMs for a load that
		// is running. Returns whether none is running.
		bool Shutdown(unsigned waitMs);

		struct State;   // shared with the posted job; defined in the .cpp

	private:
		TextureOps m_ops;
		ThumbnailTexturesHooks m_hooks;
		std::shared_ptr<State> m_state;
	};
}
