#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "auto_update/mod_index_fetcher.h"
#include "auto_update/readme_textures.h"

#include <memory>

// ---------------------------------------------------------------------------
// BrowseThumbnails -- the pictures of the Browse cards. auto_update/thumbnail_fetcher.h
// gets and checks the bytes, this turns them into textures and gives the card
// layout one call per card.
//
// Render thread: Sync, Get, Retry. Nothing here does I/O, hashing or a decode:
// the worker thread and the game-thread job (the same hop the README pictures
// use) do. Shutdown runs at loader shutdown, before the ImGui host is unloaded.
// ---------------------------------------------------------------------------

namespace UI::BrowseThumbnails
{
	// What a card shows in its picture box.
	enum class View
	{
		None,       // the entry has no picture, or the list is not signed: "No picture"
		Loading,    // "Loading..."
		Ready,      // draw `handle` at `width` x `height` (ModIndex::FitPicture to fit)
		Failed,     // "Picture unavailable"
		Mismatch,   // "Picture unavailable" (the size or hash differs; logged)
	};

	struct Thumb
	{
		View  view   = View::None;
		void* handle = nullptr;   // Ready only; valid for this frame only
		int   width  = 0;         // Ready only: the texture's size in pixels
		int   height = 0;
	};

	// The list the cards come from. Cheap when `snap` is the one given last time,
	// so it may be called every frame.
	void Sync(const std::shared_ptr<const ModIndex::Snapshot>& snap);

	// The picture state of a card whose picture box is on screen this frame.
	// Starts the download and the texture on the first calls.
	Thumb Get(const ModIndex::Entry& entry);

	// The Retry button of the list: pictures that failed are asked for again.
	void Retry();

	// Frees every texture, stops the worker.
	void Shutdown();

	// The loader's texture calls for pictures (load, free, size, post to the game
	// thread). `take` is left empty; each user sets its own.
	ModIndex::TextureOps HostTextureOps();
}

#endif // MODLOADER_CLIENT_BUILD
