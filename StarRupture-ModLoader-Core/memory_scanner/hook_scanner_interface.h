#pragma once

#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
    // The scan table handed to a plugin's OnPluginLoadHooks export, and the only
    // pattern-scanning API a plugin ever sees. Every entry checks that the caller
    // has a scan session open (PluginHookReport) and refuses otherwise, so the
    // pointer is useless once the event has returned.
    IPluginHookScanner* GetPluginHookScanner();
}
