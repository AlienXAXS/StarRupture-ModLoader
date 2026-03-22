#pragma once

#include <windows.h>
#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
    // Snapshot of a single plugin's state, safe to read after FreeLibrary.
    struct PluginStatus
    {
        char name[64];
        char version[32];
        char author[64];
        bool isLoaded;
    };

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

    // Copy status snapshots for ALL plugin records (loaded and unloaded) into out.
    // Returns total record count (may exceed maxCount).
    int GetAllPluginStatuses(PluginStatus* out, int maxCount);

    // Unload the plugin at index: calls PluginShutdown + FreeLibrary.
    // The record is kept so it can be reloaded later.
    // Returns false if index is out of range or the plugin is already unloaded.
    bool UnloadPlugin(int index);

    // Reload the plugin at index: unloads it (if currently loaded) then
    // loads it again from the same file path.
    // Returns false if index is out of range or the DLL fails to load/init.
    bool ReloadPlugin(int index);
}
