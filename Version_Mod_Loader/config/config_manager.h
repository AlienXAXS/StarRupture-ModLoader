#pragma once

#include <windows.h>
#include "plugins/plugin_interface.h"  // Use the global interface definition

namespace ModLoaderLogger
{
    // Initialize the config manager
    void InitializeConfigManager();

    // Shutdown the config manager
    void ShutdownConfigManager();

    // Get the config directory path
    const wchar_t* GetConfigDirectory();

    // Get the global config interface (uses the interface from plugin_interface.h)
    IPluginConfig* GetPluginConfig();

    // Returns the ConfigSchema registered by the plugin, or nullptr if none.
    const ConfigSchema* GetPluginSchema(const char* pluginName);
}
