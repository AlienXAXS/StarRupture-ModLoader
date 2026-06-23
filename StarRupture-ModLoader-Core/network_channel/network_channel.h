#pragma once

#include "../plugins/plugin_interface.h"

// ============================================================
// NetworkChannel -- implements IPluginNetworkChannel (v18)
//
// Server build:  sends Server->Client packets via APlayerController::ClientMessage
//                ProcessEvent call (without FUNC_Native so UE replicates to the client).
//                Receives Client->Server packets via the ServerChatCommit ProcessEvent hook.
//
// Client build:  hooks UObject::ProcessEvent globally; filters calls targeting the
//                ClientMessage UFunction; dispatches tagged payloads to registered
//                plugin handlers.
//                Sends Client->Server packets via ACrPlayerControllerBase::ServerChatCommit
//                ProcessEvent call (without FUNC_Native so UE replicates to the server).
//
// Generic build: all send/receive operations are no-ops; Network pointer in IPluginHooks
//                is nullptr.
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

#ifdef MODLOADER_SERVER_BUILD
    // Called by the server_chat_commit ProcessEvent hook with the sender UObject* and the
    // raw wchar_t payload from the FString Text parameter.
    // Returns true if the message was a mod envelope and was consumed (caller should suppress
    // the original call); returns false if the message is normal chat (caller must forward).
    bool DispatchServerMessage(void* senderUObject, const wchar_t* str, int numCharsWithNull);
#endif

} // namespace NetworkChannel
