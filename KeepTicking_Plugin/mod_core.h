#pragma once

#include "plugin_interface.h"

// Core mod logic — this is where you define your patterns, hooks, and patches.
// Called once from DllMain after the proxy is initialized.

namespace ModCore
{
    // Run on a background thread after DLL load.
    // Scans for patterns and installs hooks/patches.
    void Initialize(IPluginScanner* scanner, IPluginHooks* hooks);

    // Cleanup — remove hooks, restore patches.
    void Shutdown();
}
