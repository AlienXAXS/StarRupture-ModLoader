#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// ACrGameModeBase::Logout Hook  (Player Left)
//
// Purpose: Fires when a player controller is about to be destroyed on the
//          server — i.e. the player has disconnected, been kicked, or the
//   session is ending.  The controller is still valid at call time,
//        so plugins can safely read state from it (player name, net ID,
//          possessed pawn, etc.).
//
// Hook point:
//   ACrGameModeBase::Logout(ACrGameModeBase* this, AController* Exiting)
//
// Plugins receive the raw AController* as void* and can cast to
// SDK::AController* or SDK::ACrPlayerControllerBase*.
//
// IMPORTANT: Plugin callbacks are invoked BEFORE the original Logout runs
// so the controller is still fully valid.  After the original returns the
// controller will be destroyed.
// ---------------------------------------------------------------------------

namespace Hooks::PlayerLeft
{
	// Callback signature for plugins.
	// Receives the AController* of the departing player (as void*).
	using PluginPlayerLeftCallback = void(*)(void* exitingController);

	// Install the hook
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Register a plugin callback to be notified when a player leaves
	void RegisterPluginCallback(PluginPlayerLeftCallback callback);

	// Unregister a plugin callback
	void UnregisterPluginCallback(PluginPlayerLeftCallback callback);
}
