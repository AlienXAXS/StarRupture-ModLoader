#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// PluginPanelRegistry
//
// Thread-safe storage for plugin-registered ImGui panels.
// Plugins call hooks->UI->RegisterPanel to add a panel; the modloader renders
// a button per panel and calls the plugin's renderFn each frame while open.
// ---------------------------------------------------------------------------

namespace UI::PluginPanelRegistry
{
    // Register a panel.  desc and all strings it points to must remain valid
    // until UnregisterPanel is called.
    void RegisterPanel(const PluginPanelDesc* desc);

    // Remove a panel by window title.
    void UnregisterPanel(const char* windowTitle);

    // Register/unregister a config-change notification callback.
    void RegisterOnConfigChanged(PluginConfigChangedCallback callback);
    void UnregisterOnConfigChanged(PluginConfigChangedCallback callback);

    // Called by modloader_window to fire config-change notifications.
    void FireConfigChanged(const char* section, const char* key, const char* newValue);

    // Renders a row of "Open" buttons for all registered panels.
    // Call from inside an ImGui window; handles Begin/End for each panel window.
    void RenderPanelButtons(IModLoaderImGui* imgui);

    // Renders all open panel windows.  Call once per frame at the top level
    // (outside any other ImGui window).
    void RenderPanelWindows(IModLoaderImGui* imgui);
}

#endif // MODLOADER_CLIENT_BUILD
