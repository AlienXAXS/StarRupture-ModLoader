#pragma once

#include "plugin_interface.h"

// Wrapper around IPluginHooks for convenient usage throughout the plugin
namespace PluginHooks
{
    // Initialize with the hooks interface from the loader
    void Initialize(IPluginHooks* hooks);

    // Install an inline hook - returns handle or nullptr on failure
    HookHandle InstallHook(uintptr_t targetAddress, void* detourFunction, void** originalFunction);

    // Remove a hook
    void RemoveHook(HookHandle handle);

    // Check if hook is installed
    bool IsHookInstalled(HookHandle handle);

    // Memory patching utilities
    bool PatchMemory(uintptr_t address, const uint8_t* data, size_t size);
    bool NopMemory(uintptr_t address, size_t size);
    bool ReadMemory(uintptr_t address, void* buffer, size_t size);

    // Register for world begin play events (ChimeraMain world only)
    void RegisterWorldBeginPlayCallback(void (*callback)(SDK::UWorld*));

    // Unregister world begin play callback
    void UnregisterWorldBeginPlayCallback(void (*callback)(SDK::UWorld*));
}
