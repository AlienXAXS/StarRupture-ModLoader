#pragma once

#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
    // Get the global logger interface for plugins
    IPluginLogger* GetPluginLogger();
}
