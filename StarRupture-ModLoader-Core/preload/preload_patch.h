#pragma once

#include "preload/preload_interface.h"

struct IPluginSelf;

// The IPreloadPatch table handed to PreloadInit, and the hook registry behind
// it. Hooks are owned here rather than by the plugin so the loader can take
// them all down at shutdown -- a detour left installed after its module is
// unmapped is a jump into freed memory, and by then nothing is left to notice.
namespace PreloadPatch
{
    IPreloadPatch* GetTable();

    // Removes every hook installed by this owner and forgets them. Called by
    // the preload manager before it frees a plugin's module, and for every
    // plugin at shutdown.
    int RemoveAllHooksFor(const IPluginSelf* owner);

    // How many hooks this owner currently has installed, for the record shown
    // by the console.
    int CountHooksFor(const IPluginSelf* owner);

    // Removes every hook from every owner. Shutdown only.
    void RemoveAllHooks();
}
