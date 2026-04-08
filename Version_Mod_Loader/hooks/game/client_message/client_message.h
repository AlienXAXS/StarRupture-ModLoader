#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"

// ---------------------------------------------------------------------------
// ClientMessage hook  (client builds only)
//
// Hooks UFunction::ExecFunction on the resolved ClientMessage UFunction via
// MinHook.  This intercepts both paths:
//   - Local call:    UObject::ProcessEvent -> FFrame -> ExecFunction (hooked)
//   - Network RPC:   FObjectReplicator::ReceivedRPC -> FFrame -> ExecFunction (hooked)
//
// Incoming FString payload is read from FFrame::Locals (offset 0x20) and
// forwarded to NetworkChannel for [MOD:...] envelope parsing and plugin
// handler dispatch.
// ---------------------------------------------------------------------------

namespace Hooks::ClientMessage
{
    // Register the ProcessEvent observer.  Returns true on success.
    bool Install();

    // Unregister the observer.
    void Remove();

    // Returns true if the observer is currently registered.
    bool IsInstalled();
}

#endif // MODLOADER_CLIENT_BUILD
