#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// UGameEngine::Tick Hook
//
// Purpose: Fires every frame on the game thread.  Plugins can register
//     callbacks here to perform periodic work that must run on the
//      game thread (e.g. draining task queues, polling state, etc.)
//
// Hook point:
//   UGameEngine::Tick(UGameEngine* this, float DeltaSeconds, bool bIdleMode)
//
// Performance note: callbacks are invoked every frame — keep them fast.
// ---------------------------------------------------------------------------

namespace Hooks::EngineTick
{
    // Callback signature for plugins
    typedef void (*PluginEngineTickCallback)(float deltaSeconds);

// Install the hook
    bool Install();

    // Remove the hook
    void Remove();

    // Returns true if the hook is currently installed
    bool IsInstalled();

    // Register a plugin callback to be notified every game-thread tick
    void RegisterPluginCallback(PluginEngineTickCallback callback);

    // Unregister a plugin callback
  void UnregisterPluginCallback(PluginEngineTickCallback callback);
}
