#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// AActor::BeginPlay Hook
//
// Purpose: Fires when ANY actor begins play.  Plugins can register callbacks
//          to observe or react to actor spawns.
//
// Hook point:
//   AActor::BeginPlay(AActor* this)
//
// Performance note: this fires for EVERY actor that begins play — keep
// callbacks fast.  For diagnostics, use TRACE-level logging.
// ---------------------------------------------------------------------------

namespace Hooks::ActorBeginPlay
{
    // Callback signature for plugins.
 // Receives the raw AActor* pointer (as void*) — plugins cast to SDK::AActor*.
    typedef void (*PluginActorBeginPlayCallback)(void* actor);

    // Install the hook
 bool Install();

    // Remove the hook
    void Remove();

    // Returns true if the hook is currently installed
    bool IsInstalled();

    // Register a plugin callback to be notified when any actor begins play
    void RegisterPluginCallback(PluginActorBeginPlayCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginActorBeginPlayCallback callback);
}
