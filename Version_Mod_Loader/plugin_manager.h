#pragma once

#include <windows.h>

namespace ModLoader
{
    // Initialize the plugin manager
    void InitializePluginManager();

    // Shutdown the plugin manager
    void ShutdownPluginManager();

    // Load all plugins from the alienx_mods directory
    void LoadAllPlugins();

    // Unload all plugins
    void UnloadAllPlugins();
}
