#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// UCrExperienceManagerComponent::OnExperienceLoadComplete Hook
//
// Purpose: Fires when the experience (map/gameplay) has fully loaded —
//          significantly later than OnSaveLoaded, making it a reliable
//          trigger for "map fully ready" logic where all actors are spawned.
//
// Hook point:
//   UCrExperienceManagerComponent::OnExperienceLoadComplete(
//       UCrExperienceManagerComponent* this)
// ---------------------------------------------------------------------------

namespace Hooks::ExperienceLoadComplete
{
    // Callback signature for plugins
    typedef void (*PluginExperienceLoadCompleteCallback)();

    // Install the hook
    bool Install();

    // Remove the hook
    void Remove();

    // Returns true if the hook is currently installed
    bool IsInstalled();

    // Register a plugin callback to be notified when the experience finishes loading
    void RegisterPluginCallback(PluginExperienceLoadCompleteCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginExperienceLoadCompleteCallback callback);
}
