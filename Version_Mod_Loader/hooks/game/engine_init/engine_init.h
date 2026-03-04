#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Engine Initialization Hooks
// 
// Purpose: Engine ready signal - fires when UE5 engine finishes initialization
// Uses multiple hook points as fallbacks for maximum compatibility:
//   1. FEngineLoop::Init (primary)
//   2. UGameEngine::Init (fallback)
// ---------------------------------------------------------------------------

namespace Hooks::EngineInit
{
    // Original function signatures
    typedef int32_t (__fastcall *FEngineLoop_Init_t)(void* thisPtr);
    typedef bool (__fastcall *UGameEngine_Init_t)(void* thisPtr, void* InEngineLoop);

    // Callback signature for plugins
    typedef void (*PluginEngineInitCallback)();

    // Install the hooks (tries multiple patterns for reliability)
    // Returns true if at least one hook was successful
    bool Install();

    // Remove all hooks
    void Remove();

    // Check if engine has initialized
    bool IsEngineInitialized();

    // Register a plugin callback to be notified when engine initializes
    void RegisterPluginCallback(PluginEngineInitCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginEngineInitCallback callback);

    // Legacy compatibility - set single callback (deprecated, use RegisterPluginCallback)
    void SetEngineInitCallback(void (*callback)());
}
