#pragma once

#include "../plugins/plugin_interface.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// The game's UI state for plugins (v74, client builds only)
//
// Backs IPluginGameMenu::GetUIState and RegisterOnUIStateChanged. The loader
// tick (client_ui.cpp) composes the PluginUIState bits once per game tick
// from detectors the loader already runs -- the world events, the pause-menu
// poll and the cursor poll -- plus the menu-layer read
// (Hooks::GameMenu::ReadMenuLayer), and hands them to Publish. The menu-layer
// read is the only part that costs anything, so it runs only while
// MenuReadWanted says a registration asks for a menu bit. GetUIState never
// reports the menu bits. A menu bit stays set until its screen is fully gone,
// close animation included.
//
// Callbacks are raw pointers into plugin DLLs: ForgetModule drops a plugin's
// registrations before FreeLibrary, and PluginGuard skips a callback whose
// module is gone or switched off.
// ---------------------------------------------------------------------------

namespace UIState
{
	// The bits read from the menu layer, which are 0 while nobody wants them.
	// A registration asks for the read only if its mask has one of these.
	inline constexpr uint64_t kMenuBits = PLUGIN_UI_ANY_MENU | PLUGIN_UI_MAP | PLUGIN_UI_INVENTORY
		| PLUGIN_UI_BUILD_MENU | PLUGIN_UI_STATION_MENU;

	// The kinds of screen that have a bit of their own on the menu layer.
	enum class MenuKind { Other, Map, Inventory, BuildMenu, StationMenu };

	// The menu bits for a menu layer holding widgetCount widgets. kinds names the
	// first kindCount of them. A widget that is closing stays on the list until its
	// transition has ended, so its bits stay set through the close animation:
	// ANY_MENU while the list is not empty, and the bit of each kind on it.
	uint64_t MenuBits(size_t widgetCount, const MenuKind* kinds, size_t kindCount);

	// IPluginGameMenu::GetUIState: the state without kMenuBits. Any thread.
	uint64_t Get();

	// IPluginGameMenu::RegisterOnUIStateChanged / UnregisterOnUIStateChanged.
	// Any thread.
	void Register(uint64_t mask, PluginUIStateCallback callback, void* userData);
	void Unregister(PluginUIStateCallback callback, void* userData);

	// True while a registration's mask has a menu bit. Game thread, once per
	// tick.
	bool MenuReadWanted();

	// Stores the tick's state and calls every callback whose bits changed.
	// Game thread, once per tick.
	void Publish(uint64_t state);

	// Drop every registration whose callback lives in this module. Called by
	// the plugin manager before FreeLibrary (ForgetRegistrations).
	void ForgetModule(HMODULE module);
}
