#pragma once

#include "../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// FEngineLoop::Init Hook
// 
// Purpose: Engine ready signal - fires when UE5 engine finishes initialization
// ---------------------------------------------------------------------------

namespace Hooks::EngineInit
{
    // Original function signature
    typedef int32_t (__fastcall *FEngineLoop_Init_t)(void* thisPtr);

    // Install the hook
    // Returns true if successful, false if pattern not found or hook failed
    bool Install();

    // Remove the hook
    void Remove();

  // Check if engine has initialized
    bool IsEngineInitialized();

    // Set callback to be called when engine initializes
    void SetEngineInitCallback(void (*callback)());
}
