#pragma once

#include "../plugins/plugin_interface.h"

#include <string>
#include <vector>

// ============================================================
// NetworkChannel -- implements IPluginNetworkChannel
//
// One transport: the Unreal per-connection control channel. Payloads travel as
// control bunches led by a reserved message type, handled by
// hooks/game/control_channel/. There is no fallback -- the previous transport,
// which smuggled envelopes through the ClientSaveStringToTxt and
// ServerExecuteConsoleCommand RPCs, has been removed.
//
// Two directions, and which are live is a *runtime* question on client builds,
// because a listen host runs a client build while being the server for its own
// session:
//
// Authority direction (compiled into server AND client builds):
//                sends to clients, receives from clients. Every entry point
//                checks HasNetAuthority() -- true on a dedicated server, and on
//                a client build only when GetNetMode() reports ListenServer.
//                On a pure client (and in Standalone) these log and do nothing.
//
// Client direction (client builds only):
//                sends to the server, receives from the server.
//
// On a listen host both are live at once, and the host's own player controller
// is never a send target: it has no UNetConnection, because a message to it
// would not be a network send at all. "All clients" therefore means all remote
// clients on both builds. Host-side code already holds the authoritative data
// and can call its own handler directly.
//
// OPERATIONAL NOTE: because there is no fallback, a session requires every peer
// to run a loader whose six control-channel patterns resolved. Sending to a peer
// without a working wire does not degrade -- the engine closes that connection
// on an unrecognised control message. If our own natives fail to resolve we go
// silent instead, so a failed preflight disables plugin networking rather than
// disconnecting anyone.
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
    // Installs the control-channel detour. Must be called after GObjects is live
    // (i.e. after the engine-init hook fires).
    void Initialize();

    // Called on DLL detach / engine shutdown.
    void Shutdown();

    // -----------------------------------------------------------------------
    // Client plugin manifests (authority side)
    //
    // A joining client volunteers the list of plugins it has loaded, with their
    // versions. The authority records it per connection and uses it to decide
    // who a plugin's packets are worth sending to: a packet from plugin P
    // version V only goes to clients that reported P at exactly V.
    //
    // The client must volunteer this -- the authority cannot ask. A request
    // would be a control bunch, and sending one to a peer that is not running
    // the loader disconnects it. Which is also why a client with no manifest
    // receives nothing: silence is the only safe default, and it happens to
    // shield vanilla clients from being dropped by a broadcast.
    // -----------------------------------------------------------------------

    struct RemotePlugin
    {
        std::string name;
        std::string version;
    };

    struct ClientManifest
    {
        void*       connection = nullptr;  // UNetConnection*, identity only
        std::string playerName;            // best effort; empty if not resolvable yet
        bool        reported = false;      // false = never sent a manifest
        std::vector<RemotePlugin> plugins;
    };

    // Every client connection the authority currently knows about. Empty on a
    // pure client and on generic builds.
    std::vector<ClientManifest> GetClientManifests();

    // -----------------------------------------------------------------------
    // Fragmentation self-test (client side)
    //
    // Sends `bytes` of position-dependent filler to the authority, which bounces
    // it back verbatim. Exercises the full path in both directions: chunking on
    // send, reassembly on the authority, chunking again for the reply, reassembly
    // here -- then verifies every byte.
    //
    // Returns false immediately (with a reason in err) if the test cannot start.
    // Success only means it was sent: the PASS or FAIL verdict is asynchronous and
    // lands in modloader.log at INFO/ERROR when the reply arrives.
    // -----------------------------------------------------------------------
    bool StartFragmentationTest(size_t bytes, std::string& err);

} // namespace NetworkChannel
