#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// ModLoaderWindow
//
// The main modloader UI window toggled by the configured key (default F2).
// Contains three tabs: Plugins, Config, About.
// Rendered every frame; renders nothing when closed.
// ---------------------------------------------------------------------------

namespace UI::ModLoaderWindow
{
    // Toggle open/closed state.  Registered as the keybind callback.
    void Toggle();

    // Render this frame.  Call inside an active ImGui frame.
    // imgui: the IModLoaderImGui function table for plugin panel callbacks.
    void Render(IModLoaderImGui* imgui);

    // Returns true when the window is open (used to block game input).
    bool IsOpen();

    // Called by plugin_manager after each plugin's PluginInit completes.
    // Reads the plugin's INI and applies any <key>Blocking flags to the
    // runtime blocking map so keybind suppression is active from the first
    // key event, not deferred until the config panel is first opened.
    void LoadBlockingStateForPlugin(const char* pluginName);
}

#endif // MODLOADER_CLIENT_BUILD
