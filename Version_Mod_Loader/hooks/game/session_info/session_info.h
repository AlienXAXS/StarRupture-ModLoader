#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../../plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// Session Info  (client builds only)
//
// Purpose: Exposes query functions so plugins can determine whether the client
//          is in a solo/offline session or connected to a multiplayer server.
//
// No hook is installed. Fields are read directly from UCrSessionSubsystem
// obtained via UWorld -> OwningGameInstance -> GetGameInstanceSubsystem.
//
//   CommonSessionOnlineMode @ 0x006B  (Offline=0, LAN=1, Online=2)
//   bIsServer               @ 0x006D  (bool, confirmed via IDA Pro)
// ---------------------------------------------------------------------------

namespace Hooks::SessionInfo
{
    // Returns the current session online mode from UCrSessionSubsystem.
    // Returns Unknown if the subsystem is unavailable (e.g. called too early).
    EPluginSessionOnlineMode GetSessionOnlineMode();

    // True if GetSessionOnlineMode() is LAN or Online.
    bool IsMultiplayer();

    // True if UCrSessionSubsystem::bIsServer is set.
    bool IsServer();
}

#endif // MODLOADER_CLIENT_BUILD
