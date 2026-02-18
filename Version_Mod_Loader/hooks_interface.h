#pragma once

#include "plugin_interface.h"

namespace ModLoader
{
    // Get the global hooks interface for plugins
    IPluginHooks* GetPluginHooks();
}
