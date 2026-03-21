#pragma once

#include <windows.h>
#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
    // Initialize the plugin manager
    void InitializePluginManager();

    // Shutdown the plugin manager
    void ShutdownPluginManager();

    // Load all plugins from the Plugins directory
    void LoadAllPlugins();

    // Unload all plugins
    void UnloadAllPlugins();

    // Get the number of currently loaded plugins
    int GetLoadedPluginCount();

    // Copy up to maxCount PluginInfo pointers into outInfos.
    // Returns the total number of loaded plugins (may exceed maxCount).
    // Returned pointers remain valid as long as plugins are loaded.
    int GetLoadedPluginInfos(const PluginInfo** outInfos, int maxCount);
}
