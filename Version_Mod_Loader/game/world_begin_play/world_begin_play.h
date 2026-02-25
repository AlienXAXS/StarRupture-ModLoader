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
    // Callback signature for ChimeraMain-only notifications (existing)
    typedef void (*PluginWorldBeginPlayCallback)(SDK::UWorld* world);

    // Callback signature for ALL worlds — receives the world pointer and its name
    typedef void (*PluginAnyWorldBeginPlayCallback)(SDK::UWorld* world, const char* worldName);

    // Install the hook
    bool Install();

    // Remove the hook
    void Remove();

    // Returns true if the hook is currently installed
    bool IsInstalled();

    // Get call count
    long GetCallCount();

    // Register a plugin callback to be notified when ChimeraMain world begins play
    void RegisterPluginCallback(PluginWorldBeginPlayCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginWorldBeginPlayCallback callback);

    // Register a plugin callback to be notified when ANY world begins play.
    // The hook is installed lazily on the first registration if not already active.
    void RegisterAnyWorldCallback(PluginAnyWorldBeginPlayCallback callback);

    // Unregister an any-world callback
    void UnregisterAnyWorldCallback(PluginAnyWorldBeginPlayCallback callback);
}
