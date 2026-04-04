#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// UObject::ProcessEvent Hook  (client builds only)
//
// Purpose: Intercepts every call to UObject::ProcessEvent on the client,
//          checking for calls targeting APlayerController::ClientMessage.
//          When found, forwards the FString payload to NetworkChannel for
//          [MOD:...] envelope parsing and plugin handler dispatch.
//
// Hook point:
//   UObject::ProcessEvent(UFunction* Function, void* Parms)
//   Hooked at the absolute address: GetImageBase() + Offsets::ProcessEvent
//
// The hook is installed lazily -- only when a plugin registers a handler via
//   hooks->Network->RegisterMessageHandler().
// ---------------------------------------------------------------------------

namespace Hooks::ClientMessage
{
    // Install the ProcessEvent hook.  Returns true on success.
    bool Install();

    // Remove the hook.
    void Remove();

    // Returns true if the hook is currently installed.
    bool IsInstalled();
}

#endif // MODLOADER_CLIENT_BUILD
