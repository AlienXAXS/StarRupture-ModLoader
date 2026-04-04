#pragma once

#include "../plugins/plugin_interface.h"

// ============================================================
// NetworkChannel -- implements IPluginNetworkChannel (v17)
//
// Server build:  maintains the connected-player list via PlayerJoined/Left hooks;
//                sends packets via APlayerController::ClientMessage ProcessEvent call
//                (without FUNC_Native flag so UE replicates to the owning client).
//
// Client build:  hooks UObject::ProcessEvent globally; filters calls targeting the
//                ClientMessage UFunction; dispatches tagged payloads to registered
//                plugin handlers.
//
// Generic build: both send and receive no-op; Network pointer in IPluginHooks is nullptr.
// ============================================================

namespace NetworkChannel
{
    // Called from hooks_interface.cpp to obtain the IPluginNetworkChannel* for IPluginHooks.
    // Returns nullptr on generic (plain Debug/Release) builds.
    IPluginNetworkChannel* GetInterface();

    // Called by hooks_interface.cpp during engine-init on server+client builds.
    // Must be called after GObjects is live (i.e. after engine-init hook fires).
    void Initialize();

    // Called on DLL detach / engine shutdown.
    void Shutdown();

#ifdef MODLOADER_CLIENT_BUILD
    // Called by the client_message ProcessEvent hook with the raw wchar_t payload
    // from the FString S parameter.  Parses the [MOD:...] envelope and dispatches
    // to registered plugin handlers.
    void DispatchClientMessage(const wchar_t* str, int numCharsWithNull);
#endif

} // namespace NetworkChannel
