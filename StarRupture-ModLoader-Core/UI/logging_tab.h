#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Logging tab of the mod loader window.
//
// Owns everything about what gets written to a log file:
//   - the mod loader's own minimum level      (persisted, modloader.ini)
//   - the game's UE log verbosity             (persisted, modloader.ini)
//   - a per-plugin minimum level for each     (session only, never persisted)
//
// The per-plugin levels live in logging/plugin_log_levels.h; see that header
// for why they are deliberately not written to disk. Nothing on this screen
// writes anything the loader did not already write before this tab existed.
//
// Lives in its own file rather than as another RenderXxxTab() in
// modloader_window.cpp because it is the only tab with a table of live
// per-plugin state to keep in sync.
// ---------------------------------------------------------------------------

namespace UI::LoggingTab
{
    // Draws the tab body at the current cursor. Call from inside the mod
    // loader window's tab-content child.
    void Render();
}

#endif // MODLOADER_CLIENT_BUILD
