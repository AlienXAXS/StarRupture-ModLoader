#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "auto_update/mod_index.h"

// ---------------------------------------------------------------------------
// ReadmeView -- the README page of one Browse entry, drawn in place of the
// Browse list (auto_update/readme_fetcher.h has the text and the picture bytes,
// auto_update/readme_textures.h turns the bytes into textures).
//
// Render thread: Open, Close, Draw, BeginFrame. The page ends by itself the first
// frame after it was not drawn (another tab, the window closed or hidden), see
// BeginFrame, and every texture it held is freed then.
// ---------------------------------------------------------------------------

namespace UI::ReadmeView
{
	// Opens the page for `entry` (it needs a readmeUrl) and starts the request.
	void Open(const ModIndex::Entry& entry);

	// Leaves the page: closes the request and frees every texture.
	void Close();

	bool IsOpen();

	// Draws the page inside the Browse tab's content. Only while IsOpen().
	void Draw();

	// Once per frame, before the window decides what to draw: ends a page that was
	// not drawn on the previous frame.
	void BeginFrame();

	// Loader shutdown, before the ImGui host is unloaded: frees the textures and
	// stops further loads.
	void Shutdown();
}

#endif // MODLOADER_CLIENT_BUILD
