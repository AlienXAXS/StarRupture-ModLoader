#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"
#include <cstddef>

// ---------------------------------------------------------------------------
// ModLoader hello hook  (client builds only)
//
// Receives the authority's greeting, which is the only thing that tells a client
// it is safe to speak on the control channel.
//
// The greeting rides APlayerController::ClientMessage -- an ordinary replicated
// engine RPC -- because it is the one message that reaches a client which may not
// be running the loader at all without doing it any harm. On a shipping build
// ClientMessage ends at ViewportConsole->OutputText, and shipping never creates a
// ViewportConsole (all the glue that would is compiled out), so on a vanilla
// client it allocates a string, finds a null console and returns. Nothing on
// screen, nothing on disk, nothing in the log.
//
// It cannot be a control bunch. That is the whole point: a bunch carrying our
// reserved message type is what closes the connection of a peer that does not
// understand it, so the message which asks "do you understand it?" must travel
// somewhere else.
//
// Both interception paths from the old ClientSaveStringToTxt transport are kept:
//   ExecFunction hook:     FObjectReplicator::ReceivedRPC -> Invoke -> exec thunk
//   ProcessEvent observer: whenever the game routes through UObject::ProcessEvent
// Either one firing is enough, and both firing is harmless -- the greeting only
// sets a flag, so a duplicate is idempotent. That is why two paths are affordable
// here when they would not have been for a data transport.
// ---------------------------------------------------------------------------

namespace Hooks::ModLoaderHello
{
	// payload is whatever followed the sentinel (the authority's build tag), and
	// is only valid for the duration of the call.
	using HelloCallback = void(*)(const wchar_t* payload);

	// Resolves APlayerController::ClientMessage and hooks its exec thunk.
	// Requires GObjects to be live, so call after engine init.
	bool Install();

	void Remove();
	bool IsInstalled();

	// Set before Install() so a greeting arriving on the very first RPC is not lost.
	void SetCallback(HelloCallback cb);
}

#endif // MODLOADER_CLIENT_BUILD
