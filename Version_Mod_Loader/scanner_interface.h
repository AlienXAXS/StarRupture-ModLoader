#pragma once

#include "plugin_interface.h"

namespace ModLoader
{
    // Get the global scanner interface for plugins
    IPluginScanner* GetPluginScanner();
}
