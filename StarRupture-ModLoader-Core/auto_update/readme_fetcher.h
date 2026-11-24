#pragma once

#include "auto_update/readme_images.h"
#include "auto_update/readme_markdown.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// readme_fetcher.h -- fetches one mod's README (readme_markdown.h) and its
// pictures on a worker thread, and holds them in memory for the README view.
// Nothing is written to disk.
//
// The view, once per frame:
//
//   auto snap = ModIndex::ReadmeInstance().Get();    // no I/O, no allocation
//   switch (snap->state) {
//   case ReadmeState::Idle:    nothing open
//   case ReadmeState::Loading: ...draw "Loading..."
//   case ReadmeState::Failed:  ...draw snap->error
//   case ReadmeState::Ready:   ...draw snap->document->blocks; an Image block
//                              with imageSlot >= 0 has its state in
//                              snap->images[imageSlot]: Pending/Loading: a
//                              placeholder, Failed: the alt text, Bytes: take the
//                              bytes once (TakeImageBytes) on the game thread,
//                              decode, and keep the texture
//   }
//
// Threads: Open, Close, Get, TakeImageBytes and BytesHeld may be called from any
// thread, the render thread included: they take a short lock and never wait on
// the network. One worker thread runs the requests, one after the other (the
// README, then each picture in turn). A snapshot is never changed after it is
// published, so a shared_ptr the view holds stays valid and consistent.
//
// Open starts a request for one entry; it replaces whatever was open. Close
// drops the document and every picture byte still held, and makes any request in
// flight discard its answer. Opening again fetches again.
//
// Limits: kMaxImages pictures per README (the first, by address; a further one
// shows its alt text), kMaxImageBytes each, kMaxImageTotalBytes together, the
// README itself kMaxFetchBytes. A picture counts only when its bytes start with
// a PNG, JPEG, GIF, WebP or BMP signature; SVG, XML and HTML are refused.
//
// Shutdown: the worker owns a share of the state, so it can finish after the
// fetcher is gone. Shutdown(waitMs) stops new requests, invalidates the open
// one and waits up to waitMs for the worker (a request ends by its HttpGet
// deadline).
// ---------------------------------------------------------------------------

namespace ModIndex
{
	constexpr size_t kMaxImages          = 8;
	constexpr size_t kMaxImageBytes      = 2 * 1024 * 1024;
	constexpr size_t kMaxImageTotalBytes = 6 * 1024 * 1024;
	constexpr size_t kMaxFetchBytes      = 512 * 1024;       // the README text; the parser then cuts at kMaxReadmeBytes

	enum class ReadmeState { Idle, Loading, Ready, Failed };
	enum class ImageState  { Pending, Loading, Bytes, Failed };

	struct ImageSlot
	{
		std::string url;                    // an address that passes IsAllowedImageUrl
		ImageState  state = ImageState::Pending;
		size_t      size  = 0;              // Bytes: how many bytes were fetched
		bool        taken = false;          // Bytes: TakeImageBytes has handed them out
		std::string error;                  // Failed: plain words
	};

	struct ReadmeSnapshot
	{
		ReadmeState state = ReadmeState::Idle;
		std::string entryId;                // what Open was given; empty when Idle
		std::shared_ptr<const Document> document;   // Ready only; its Image blocks carry imageSlot
		std::vector<ImageSlot> images;      // Ready only; index = Block::imageSlot
		std::string error;                  // Failed only: plain words for the player
	};

	// Where a download comes from: the body on success, or an empty string on any
	// failure with `error` set to plain words (left empty, the fetcher words it).
	// A body longer than maxBytes counts as a failure. Runs on the worker thread.
	using ReadmeFetchFn = std::function<std::string(const std::string& url, size_t maxBytes, std::string& error)>;

	class ReadmeFetcher
	{
	public:
		explicit ReadmeFetcher(ReadmeFetchFn fetch);
		~ReadmeFetcher();
		ReadmeFetcher(const ReadmeFetcher&) = delete;
		ReadmeFetcher& operator=(const ReadmeFetcher&) = delete;

		// Replaces what is open and starts the request. A readmeUrl that fails
		// IsAllowedReadmeUrl is refused at once: the state is Failed and the answer
		// false. False also after Shutdown.
		bool Open(const std::string& entryId, const std::string& readmeUrl);

		// Back to Idle: the document and all picture bytes are dropped, and a
		// request in flight discards its answer.
		void Close();

		// The current snapshot. Never null.
		std::shared_ptr<const ReadmeSnapshot> Get() const;

		// A fetched picture's bytes, handed out once: afterwards, and for a slot
		// that is not in state Bytes, an empty string. Run the decode on the game thread.
		std::string TakeImageBytes(size_t slot);

		// Picture bytes held now (fetched, not yet taken).
		size_t BytesHeld() const;

		// No new requests after this; waits up to waitMs for the worker. Returns
		// whether it has ended.
		bool Shutdown(unsigned waitMs);

		struct Shared;   // the state the worker shares; defined in the .cpp

	private:
		std::shared_ptr<Shared> m_shared;
	};

	// The loader's one README fetcher (HttpGet, the feed's timeouts). Made on
	// first use and kept to the end of the process.
	ReadmeFetcher& ReadmeInstance();

	// ReadmeInstance().Shutdown(waitMs), for the loader's shutdown.
	bool ShutdownReadmeInstance(unsigned waitMs);
}
