#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// ACrGameModeBase::PostLogin Hook  (Player Joined)
//
// Purpose: Fires when a player controller has fully completed login on the
//       server.  At this point the controller is networked, replicated,
//  and ready — but may not yet have a possessed pawn (the game
//    normally waits for the profession selection UI).
//
// Hook point:
//   ACrGameModeBase::PostLogin(ACrGameModeBase* this, APlayerController* NewPlayer)
//
// Plugins receive the raw APlayerController* as void* and can cast to
// SDK::APlayerController* or SDK::ACrPlayerControllerBase*.
// ---------------------------------------------------------------------------

namespace Hooks::PlayerJoined
{
    // Callback signature for plugins.
    // Receives the APlayerController* of the newly-logged-in player (as void*).
    typedef void (*PluginPlayerJoinedCallback)(void* playerController);

    // Install the hook
    bool Install();

    // Remove the hook
    void Remove();

    // Returns true if the hook is currently installed
    bool IsInstalled();

    // Register a plugin callback to be notified when a player joins
    void RegisterPluginCallback(PluginPlayerJoinedCallback callback);

    // Unregister a plugin callback
    void UnregisterPluginCallback(PluginPlayerJoinedCallback callback);
}
