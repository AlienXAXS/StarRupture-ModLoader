#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// Forward declarations for SDK types (avoid including SDK headers in mod loader)
namespace SDK
{
 class UObject;
    class UWorld;
}

// ---------------------------------------------------------------------------
// UCrSessionWorldLoaderSubsystem::OnWorldBeginPlay Hook
// ---------------------------------------------------------------------------

namespace Hooks::WorldBeginPlay
{
    // Callback signature for plugins
    typedef void (*PluginWorldBeginPlayCallback)(SDK::UWorld* world);

    // Install the hook
    bool Install();

    // Remove the hook
    void Remove();

    // Get call count
    long GetCallCount();

    // Register a plugin callback to be notified when ChimeraMain world begins play
    void RegisterPluginCallback(PluginWorldBeginPlayCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginWorldBeginPlayCallback callback);
}
