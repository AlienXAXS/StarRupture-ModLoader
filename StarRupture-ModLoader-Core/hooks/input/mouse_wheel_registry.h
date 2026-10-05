#pragma once

#include "../../plugins/plugin_interface.h"

#include <windows.h>

// ---------------------------------------------------------------------------
// Mouse wheel registry (v70, client builds only)
//
// Backs IPluginInputEvents::RegisterMouseWheel. Fed from the ImGui backend's
// WndProc for every WM_MOUSEWHEEL / WM_MOUSEHWHEEL, whether or not a modloader
// window has input capture -- which is the point: before this, the wheel only
// reached plugins through ImGuiIO, and ImGui is only fed the mouse while a
// window owns the cursor.
// ---------------------------------------------------------------------------

namespace Hooks::MouseWheel
{
	void Register(PluginMouseWheelCallback callback, void* userData);
	void Unregister(PluginMouseWheelCallback callback, void* userData);

	// Drop every registration whose callback lives in this module. Called by
	// the plugin manager before FreeLibrary, next to ForgetPluginSchema and
	// PluginConsole::ForgetPlugin, for the same reason: the callback is a
	// pointer into the DLL about to be unmapped.
	void ForgetModule(HMODULE module);

	// Clear all registrations.
	void Shutdown();

	// Handle a WM_MOUSEWHEEL / WM_MOUSEHWHEEL. Returns true when a handler
	// consumed it and the game must not see it -- never while uiCapturing,
	// where the return values are ignored. Any other message returns false.
	bool Dispatch(UINT msg, WPARAM wParam, LPARAM lParam, bool uiCapturing);

	// For the debug HUD.
	int GetRegistrationCount();
}
