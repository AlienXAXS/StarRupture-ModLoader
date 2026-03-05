#pragma once

#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
    // Get the global scanner interface for plugins
    IPluginScanner* GetPluginScanner();
}
