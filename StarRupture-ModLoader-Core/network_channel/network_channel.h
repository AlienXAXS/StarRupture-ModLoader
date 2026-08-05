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
// HANDSHAKE: nothing goes on the control channel until the peer has proved it
// speaks it, because sending to a peer that does not is not a dropped packet --
// it is a closed connection (ENetCloseResult::ControlChannelMessageUnknown).
// The proof cannot itself be a control bunch, so it travels out of band:
//
//   authority -> client   APlayerController::ClientMessage carrying kHelloSentinel.
//                         A replicated engine RPC, inert on a client without the
//                         loader (see hooks/game/modloader_hello/).
//   client -> authority   the plugin manifest, on the wire, sent ONLY after that
//                         greeting has been seen.
//
// Each direction therefore only ever speaks the wire to a peer that has already
// identified itself, and a peer running no loader at all is never sent anything
// it can be hurt by. A client that is never greeted stays silent forever, which
// is exactly what joining a vanilla server should look like.
//
// OPERATIONAL NOTE: because there is no fallback, a session requires every peer
// to run a loader whose six control-channel patterns resolved. Sending to a peer
// without a working wire does not degrade -- the engine closes that connection
// on an unrecognised control message.
//
// Those six patterns are therefore REQUIRED at preflight: if one goes missing
// after a game update the whole mod loader disables itself and the game starts
// unmodified, rather than booting a session in which networked plugins are
// silently inert. The IsAvailable() checks on the send paths remain as a
// last-resort guard, not as a supported degraded mode.
//
// Generic build: all send/receive operations are no-ops; Network pointer in IPluginHooks
//                is nullptr.
// ============================================================

namespace NetworkChannel
{
    // Prefix of the authority's greeting, carried in the ClientMessage string.
    // Shared by the sender (authority, both builds) and the receiving hook
    // (client builds), so it is declared unconditionally. Anything following it
    // in the string is the authority's loader build tag.
    //
    // Deliberately bracketed and unpronounceable: if it ever does reach a real
    // console on some future build, it should read as a diagnostic rather than as
    // something addressed to the player.
    inline constexpr const wchar_t* kHelloSentinel = L"[SRML-HELLO]";

    // Called from hooks_interface.cpp to obtain the IPluginNetworkChannel* for IPluginHooks.
    // Returns nullptr on generic (plain Debug/Release) builds.
    IPluginNetworkChannel* GetInterface();

    // Called from InitPluginsPhase, before any PluginInit, on every build.
    // Installs the control-channel detour, the greeting hook and the join/leave
    // callbacks. Must run after GObjects is live (it does -- that phase runs
    // after engine init completes, with the main thread held).
    //
    // Not conditional on a plugin existing: the authority must greet joining
    // clients whether or not it has plugins of its own, or those clients
    // conclude it has no mod loader. Idempotent, so the older lazy call in
    // GetPluginHooks() is now just a safety net.
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
        // Greetings sent to this client so far. Zero on a client that has either
        // answered (we stop) or never been greeted; non-zero alongside
        // reported == false is a client that is being asked and not answering,
        // which usually means it is not running the loader.
        int         greetingAttempts = 0;
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
