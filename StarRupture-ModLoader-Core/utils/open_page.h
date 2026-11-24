#pragma once

#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// open_page.h -- the one place the loader opens a web page in the player's
// browser. Only an address that passes ModIndex::IsAllowedPageUrl (https, on
// nexusmods.com or github.com) is opened; anything else is ignored. Used by the
// Browse cards and the README view's links.
//
// Call it on a click, from the render thread: it is a one-shot ShellExecute.
// ---------------------------------------------------------------------------

namespace OpenPage
{
	// Opens `url` in the default browser. Returns whether the address was allowed
	// and handed to the shell.
	bool Open(const std::string& url);

	// The same, with the shell call passed in (the argument is the address as wide
	// text). For tests.
	bool Open(const std::string& url, const std::function<void(const std::wstring&)>& opener);
}
