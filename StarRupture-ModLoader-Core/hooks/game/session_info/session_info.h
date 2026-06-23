#pragma once

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

#include "../../../plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// Net Mode Info  (server + client builds)
//
// Purpose: Exposes query functions so plugins can determine the current
//          network mode (Standalone / DedicatedServer / ListenServer / Client).
//
// Implementation: AOB-scans for AActor::InternalGetNetMode and calls it
// directly as a trampoline (no hook/detour) against any available actor.
//
//   ENetMode __fastcall AActor::InternalGetNetMode(AActor* this)
// ---------------------------------------------------------------------------

namespace Hooks::SessionInfo
{
    // Resolves the AActor::InternalGetNetMode trampoline via AOB scan.
    // Returns false if the pattern could not be found.
    bool Install();

    // Returns the current network mode by calling AActor::InternalGetNetMode
    // on an available actor. Returns Unknown if the trampoline is not resolved
    // or no actor is available yet (e.g. called before world begin play).
    EPluginNetMode GetNetMode();

    // True if GetNetMode() is ListenServer or Client.
    bool IsMultiplayer();

    // True if GetNetMode() is DedicatedServer or ListenServer.
    bool IsServer();
}

#endif // MODLOADER_SERVER_BUILD || MODLOADER_CLIENT_BUILD
