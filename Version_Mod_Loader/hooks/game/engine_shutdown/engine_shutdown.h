#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Engine Shutdown Hooks
//
// Purpose: Engine shutdown signal - fires before the UObject system tears down.
//          Plugins should use this to restore any patches or free any allocations
//          that reference engine-owned memory (UStruct chains, UObject pointers, etc.)
//
// Hook points (tried in order, only one notification fires):
//   1. FEngineLoop::Exit  (primary)
//   2. UEngine::PreExit   (fallback)
// ---------------------------------------------------------------------------

namespace Hooks::EngineShutdown
{
    // Callback signature for plugins
    typedef void (*PluginEngineShutdownCallback)();

    // Install the hook
    // Returns true if the hook was successfully installed
    bool Install();

    // Remove the hook
    void Remove();

    // Register a plugin callback to be notified when engine shuts down
    void RegisterPluginCallback(PluginEngineShutdownCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginEngineShutdownCallback callback);
}
