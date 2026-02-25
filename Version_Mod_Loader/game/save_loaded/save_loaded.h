#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// UCrMassSaveSubsystem::OnSaveLoaded Hook
//
// Purpose: Save-loaded signal - fires once the save has finished loading
//      and (hopefully) all actors have been spawned.
//
// Hook point:
//   UCrMassSaveSubsystem::OnSaveLoaded(UCrMassSaveSubsystem* this)
// ---------------------------------------------------------------------------

namespace Hooks::SaveLoaded
{
    // Callback signature for plugins
    typedef void (*PluginSaveLoadedCallback)();

    // Install the hook
  bool Install();

    // Remove the hook
    void Remove();

    // Returns true if the hook is currently installed
    bool IsInstalled();

    // Register a plugin callback to be notified when a save finishes loading
    void RegisterPluginCallback(PluginSaveLoadedCallback callback);

  // Unregister a plugin callback
    void UnregisterPluginCallback(PluginSaveLoadedCallback callback);
}
