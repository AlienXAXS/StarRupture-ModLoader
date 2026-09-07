#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// GameMenu::Registry -- the set of extra rows spliced into the game's own
// main menu and pause menu.
//
// The hook (game_menu.cpp) owns the engine side: it splices one sentinel
// ECrMenuType per live entry into the widget's LeftButtons array before the
// game builds its buttons, and swallows the click before the game's switch
// ever sees the sentinel.  This file owns everything above that line: what the
// rows say, who owns them, and what happens when one is clicked.
//
// Slots, not pointers
// -------------------
// An entry occupies a fixed slot for its whole life, and the sentinel value
// spliced into LeftButtons is kSentinelBase + slot.  That is why the slot
// count is capped at 64: ECrMenuType is a uint8 whose real values stop at 19
// (ECrMenuType_MAX), so 0xC0..0xFF is a range the game will never produce, and
// 64 values is the whole of it.  Going wider would mean using values the game
// might one day define, which is exactly the collision this range avoids.
//
// The slot is also what makes the click path stateless.  Nothing has to
// remember which UTabButton belongs to which entry -- the answer is in the
// menu widget's own LeftButtons array, which is live for as long as the widget
// that dispatches the click.  See game_menu.cpp.
//
// Callbacks are raw pointers into plugin DLLs
// -------------------------------------------
// Same hazard, and the same two defences, as the keybind registry: the loader
// drops a plugin's entries before FreeLibrary (ForgetPlugin, called from
// PluginManager next to ForgetPluginSchema), and Invoke re-checks that the
// callback address still belongs to a loaded module before calling it.  The
// try/catch around the call does not cover this -- under /EHsc an access
// violation is not a C++ exception.
// ---------------------------------------------------------------------------

namespace GameMenu::Registry
{
	// Slot capacity.  See the header comment for why it is exactly 64.
	inline constexpr int kMaxEntries = 64;

	// One row as the hook needs to see it.  The label is copied rather than
	// pointed at: the hook reads this on the game thread while a plugin may be
	// registering or being unloaded on another.
	struct EntryView
	{
		int  slot;
		int  anchor;         // PluginGameMenuAnchor
		char label[96];
	};

	// The interface handed to plugins as hooks->GameMenu.  Never null on client
	// builds; the field itself is null on server/generic builds.
	IPluginGameMenu* GetInterface();

	// Add a row owned by the loader itself rather than by a plugin.  Loader
	// entries are never dropped by ForgetPlugin -- the core DLL outlives every
	// menu -- and are not subject to the module-liveness check.
	GameMenuEntryHandle AddLoaderEntry(const PluginGameMenuEntryDesc* desc);

	// Remove a loader-owned row.
	bool RemoveLoaderEntry(GameMenuEntryHandle handle);

	// Copy every live entry targeting `target` (a PluginGameMenuTarget bit)
	// into `out`, in registration order.  Returns how many were written.
	int Snapshot(int target, EntryView* out, int maxOut);

	// True when at least one entry targets `target`.  Cheaper than Snapshot for
	// the common "nothing registered, leave the menu alone" case.
	bool HasAnyFor(int target);

	// Fire the entry in `slot`, on the calling thread (which is the game thread
	// -- the click arrives inside the menu widget's ButtonClicked).  Silently
	// ignores a slot that is no longer live, and refuses a callback whose
	// module has been unloaded.
	void Invoke(int slot);

	// Drop every entry this plugin registered.  Called by PluginManager before
	// FreeLibrary, alongside ForgetPluginSchema and PluginConsole::ForgetPlugin
	// and for the same reason: an entry left registered past that point is a
	// callback pointer into an unmapped module.
	void ForgetPlugin(const char* pluginName);
}

#endif // MODLOADER_CLIENT_BUILD
