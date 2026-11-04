#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_manager.h"

#include <vector>

// ---------------------------------------------------------------------------
// PluginScanWindow
//
// The popup that reports what LOAD NEW found: one line per DLL with its status
// and what to do about it. The plugin list only shows plugins that have a
// record, and most of what can go wrong with a DLL dropped in at runtime (a
// Preload plugin that needs a restart, a locked file, a name that is already
// taken, events a late plugin missed) leaves no row to read it from.
//
// Flow:
//   1. The LOAD NEW button posts PluginManager::ScanForNewPlugins to the game
//      thread, which calls Show() with the per-DLL results.
//   2. Render() opens the window when any result needs something from the user.
//      A scan where every plugin came up fully live opens nothing; the button's
//      result line says so.
// ---------------------------------------------------------------------------

namespace UI::PluginScanWindow
{
    // Hand over the results of one scan. Thread-safe; called from the game
    // thread. Replaces whatever the window was showing.
    void Show(std::vector<PluginManager::ScanResult> results);

    // Render this frame. Call inside an active ImGui frame; renders nothing
    // when closed.
    void Render();

    // True while the popup is open. ShouldCaptureInput() (client_ui.cpp) needs
    // this or the Close button is unclickable when no other loader window is open.
    bool IsOpen();
}

#endif // MODLOADER_CLIENT_BUILD
