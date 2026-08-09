#include "pch.h"
#include "network_channel.h"
#include "packet_fragmentation.h"
#include "logging/logger.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

// SDK headers -- paths resolved via $(StarRuptureSDKConfigDir) in Shared.props.
#include "CoreUObject_classes.hpp"  // UObject, UClass, UFunction, GObjects
#include "Engine_classes.hpp"       // UWorld, UGameplayStatics, AActor
#include "Chimera_classes.hpp"      // ACrPlayerControllerBase

// The authority side of the channel (send to clients, receive from clients) is
// compiled into both builds: a listen host runs a client build but is the server
// for its own session.  Authority is a runtime question, answered by SessionInfo.
#include "hooks/game/session_info/session_info.h"
#include "hooks/game/control_channel/control_channel.h"
#include "hooks/game/player_joined/player_joined.h"
#include "hooks/game/player_left/player_left.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/game/modloader_hello/modloader_hello.h"
#endif
#include "hooks/game/world_end_play/world_end_play.h"
#include "hooks/game/world_begin_play/world_begin_play.h"
#include "hooks/game/engine_tick/engine_tick.h"
#include "plugins/plugin_manager.h"

#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <cstdio>

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

// ============================================================
// Base64 helpers (no external dependency)
// ============================================================

namespace
{
    static constexpr const char* kB64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string Base64Encode(const uint8_t* data, size_t size)
    {
        std::string out;
        out.reserve(((size + 2) / 3) * 4);
        uint32_t val = 0;
        int valBits = -6;
        for (size_t i = 0; i < size; ++i)
        {
            val = (val << 8) | data[i];
            valBits += 8;
            while (valBits >= 0)
            {
                out.push_back(kB64Chars[(val >> valBits) & 0x3F]);
                valBits -= 6;
            }
        }
        if (valBits > -6)
            out.push_back(kB64Chars[((val << 8) >> (valBits + 8)) & 0x3F]);
        while (out.size() % 4)
            out.push_back('=');
        return out;
    }

    std::vector<uint8_t> Base64Decode(const char* data, size_t size)
    {
        static const int8_t kLut[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        };
        std::vector<uint8_t> out;
        out.reserve((size / 4) * 3);
        uint32_t val = 0;
        int valBits = -8;
        for (size_t i = 0; i < size; ++i)
        {
            if (data[i] == '=') break;
            int8_t c = kLut[static_cast<uint8_t>(data[i])];
            if (c < 0) continue;
            val = (val << 6) | static_cast<uint8_t>(c);
            valBits += 6;
            if (valBits >= 0)
            {
                out.push_back(static_cast<uint8_t>((val >> valBits) & 0xFF));
                valBits -= 8;
            }
        }
        return out;
    }

    // ============================================================
    // Envelope helpers
    // Wire format (ASCII inside FString): [MOD:pluginName:typeTag:size:]base64payload
    //   pluginName : plugin's self-reported name (must not contain ':')
    //   typeTag    : packet type string (must not contain ':')
    //   size       : decimal byte count of the decoded payload (validation guard)
    //   base64payload : standard base64, no line breaks
    // ============================================================

    static constexpr const char* kEnvPrefix = "[MOD:";
    static constexpr size_t kEnvPrefixLen   = 5; // strlen("[MOD:")

    // Build the ASCII envelope string.
    std::string BuildEnvelope(const char* pluginName, const char* typeTag,
                              const uint8_t* data, size_t size)
    {
        std::string b64 = Base64Encode(data, size);
        std::string env;
        env.reserve(kEnvPrefixLen + strlen(pluginName) + 1 + strlen(typeTag) + 1 + 10 + 2 + b64.size());
        env  = kEnvPrefix;
        env += pluginName;
        env += ':';
        env += typeTag;
        env += ':';
        char szSize[24];
        _snprintf_s(szSize, sizeof(szSize), _TRUNCATE, "%zu", size);
        env += szSize;
        env += ":]";
        env += b64;
        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] BuildEnvelope: plugin='%S' tag='%S' payload=%zu bytes base64=%zu chars total=%zu chars",
            pluginName,
            typeTag,
            size,
            b64.size(),
            env.size());
        return env;
    }

    struct ParsedEnvelope
    {
        std::string pluginName;
        std::string typeTag;
        std::vector<uint8_t> payload;
        size_t declaredSize = 0;
        size_t encodedPayloadChars = 0;
        const char* failureReason = "not_parsed";
        bool valid = false;
    };

    // Parse from a narrow-string view (converted from FString.Data on the receive path).
    ParsedEnvelope ParseEnvelope(const char* str, size_t len)
    {
        ParsedEnvelope result;
        result.failureReason = "too_short";
        if (len < kEnvPrefixLen + 4) return result;
        result.failureReason = "missing_prefix";
        if (memcmp(str, kEnvPrefix, kEnvPrefixLen) != 0) return result;

        const char* p = str + kEnvPrefixLen;
        const char* end = str + len;

        // pluginName
        const char* colon1 = static_cast<const char*>(memchr(p, ':', end - p));
        result.failureReason = "missing_plugin_separator";
        if (!colon1) return result;
        result.pluginName.assign(p, colon1);

        // The type tag may itself contain ':' (e.g. MSVC typeid names with namespaces).
        // Parse from the end by locating the final ":]" marker that terminates the size field,
        // then use the previous ':' as the typeTag/size separator.
        const char* close = nullptr;
        for (const char* scan = end - 1; scan > colon1 + 1; --scan)
        {
            if (*scan == ']' && *(scan - 1) == ':')
            {
                close = scan;
                break;
            }
        }
        result.failureReason = "missing_closing_bracket";
        if (!close) return result;

        const char* sizeColon = close - 1;
        const char* typeColon = sizeColon - 1;
        while (typeColon > colon1 && *typeColon != ':')
            --typeColon;
        result.failureReason = "missing_typetag_separator";
        if (typeColon <= colon1 || *typeColon != ':') return result;

        p = colon1 + 1;
        result.typeTag.assign(p, typeColon);

        const char* sizeStart = typeColon + 1;
        result.declaredSize = static_cast<size_t>(atoi(sizeStart));

        // base64 payload starts right after the closing ']'
        p = close + 1;
        result.encodedPayloadChars = static_cast<size_t>(end - p);
        result.payload = Base64Decode(p, static_cast<size_t>(end - p));
        result.failureReason = "payload_size_mismatch";
        if (result.payload.size() != result.declaredSize) return result;

        result.failureReason = nullptr;
        result.valid = true;
        return result;
    }

    // Shared handler key format: "pluginName\x01typeTag"
    static std::string MakeHandlerKey(const char* pluginName, const char* typeTag)
    {
        std::string key(pluginName);
        key += '\x01';
        key += typeTag;
        return key;
    }

} // anonymous namespace

// ============================================================
// Internal state
// ============================================================

// Defined near the bottom, next to the SEH read it wraps. Declared here because
// the log lines an operator actually reads -- who reported, who never answered --
// are worth a player name rather than only a pointer.
static std::string PlayerNameForConnection(void* conn);

namespace
{
    // ------------------------------------------------------------
    // Authority
    //
    // "Can I send to clients?" is a runtime question, not a build-time one.
    // A dedicated server always can.  A client build can whenever it is hosting
    // the session (listen server) -- it owns the same NetDriver a dedicated
    // server does, and its player controllers have the same client connections.
    //
    // Standalone deliberately reports false: there are no remote clients, and
    // the local-only broadcast that would result is not something a plugin
    // written against the dedicated-server model expects.
    // ------------------------------------------------------------
    static bool HasNetAuthority()
    {
#ifdef MODLOADER_CLIENT_BUILD
        // ListenServer or DedicatedServer; false for Client, Standalone, Unknown.
        return Hooks::SessionInfo::IsServer();
#else
        return true;
#endif
    }

    static bool NC_IsServer() { return HasNetAuthority(); }

// ============================================================
// Wire transport (control channel)
//
// The only transport. Payloads travel as one control bunch per message, carrying
// the ASCII envelope built below.
//
// WHY NOTHING IS SENT UNTIL THE PEER HAS IDENTIFIED ITSELF:
// UControlChannel::ReceivedBunch dispatches a leading uint8 through a switch
// covering 0x00..0x21; 0xC0 falls through to the default, and the engine's
// default is to close the connection (ENetCloseResult::ControlChannelMessageUnknown).
// Sending to a peer without a working wire does not degrade -- it DISCONNECTS
// them. Which is why v54's claim that negotiation was impossible mattered, and
// why it was too strong: negotiation is impossible IN BAND, because the probe
// would be the thing that kills the peer. Out of band it is routine.
//
// The greeting is therefore an APlayerController::ClientMessage RPC (see
// hooks/game/modloader_hello/), and the sequence is:
//
//   authority   PostLogin -> greet, retried until the client answers or we
//               conclude it is not running the loader
//   client      greeted -> report the manifest on the wire (the first bunch
//               either side sends), never before
//   authority   manifest received -> ack, and this client becomes a delivery
//               target for the plugins it named
//
// A vanilla client is greeted, does nothing with it, and is never sent a bunch.
// A loader client on a vanilla server is never greeted, so it never sends one.
// Both were disconnections before this existed.
//
// The six natives this needs are REQUIRED patterns, so a game update that moves
// one disables the whole loader at preflight rather than leaving a session where
// networked plugins quietly do nothing. The IsAvailable() check on every send is
// kept as a last-resort guard for the case where the scan passed but the hook
// install did not -- it means we go quiet rather than dropping the peers we talk
// to, but it is not a mode anything is expected to run in.
// ============================================================

    // ---- Plugin manifest ----------------------------------------------------
    //
    // Reserved envelope name for loader-internal traffic. Consumed by the
    // receive cores before any plugin lookup, so no plugin can see or spoof it.
    // Safe to carry over the wire without negotiation: the wire is the only
    // transport, so a peer that can receive this already speaks it by definition.
    static constexpr const char* kMlPlugin   = "$ML";
    static constexpr const char* kMlManifest = "manifest";
    static constexpr const char* kMlAck      = "ack";
    static constexpr const char* kMlEcho     = "echo";
    static constexpr const char* kMlEchoBack = "echoreply";

    // Deterministic, position-dependent filler for the echo test. Position-dependent
    // matters: a constant byte would pass even if the reassembler wrote chunks to
    // the wrong offsets, which is exactly the bug this test is meant to catch.
    static uint8_t EchoByteAt(size_t i)
    {
        return static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    }

    // Wire form: name US version RS, repeated. Unit/record separators rather
    // than punctuation because plugin names and versions are author-controlled
    // strings and nothing stops one containing a comma, colon or newline.
    static constexpr char kUnitSep   = '\x1f';
    static constexpr char kRecordSep = '\x1e';

    static std::mutex g_manifestMutex;
    // Keyed by UNetConnection*. An entry exists only once a client has reported.
    static std::unordered_map<void*, std::vector<NetworkChannel::RemotePlugin>> g_manifests;

    static std::string EncodeManifest()
    {
        const PluginInfo* infos[256] = {};
        const int n = PluginManager::GetLoadedPluginInfos(infos, 256);

        std::string out;
        for (int i = 0; i < n; ++i)
        {
            if (!infos[i] || !infos[i]->name) continue;
            const char* name = infos[i]->name;
            const char* ver  = infos[i]->version ? infos[i]->version : "";

            // A name or version containing a separator would corrupt the frame.
            // Drop the entry rather than emit something the peer will misparse:
            // being absent from the manifest costs that plugin its traffic, which
            // is a great deal better than shifting every entry after it.
            if (strchr(name, kUnitSep) || strchr(name, kRecordSep) ||
                strchr(ver, kUnitSep)  || strchr(ver, kRecordSep))
            {
                ModLoaderLogger::LogWarn(
                    L"[NetworkChannel] Plugin '%S' has a separator character in its name or "
                    L"version and was left out of the manifest", name);
                continue;
            }

            out += name;
            out += kUnitSep;
            out += ver;
            out += kRecordSep;
        }
        return out;
    }

    static std::vector<NetworkChannel::RemotePlugin> DecodeManifest(const uint8_t* data, size_t len)
    {
        std::vector<NetworkChannel::RemotePlugin> out;
        const char* p   = reinterpret_cast<const char*>(data);
        const char* end = p + len;

        while (p < end)
        {
            const char* rec = static_cast<const char*>(memchr(p, kRecordSep, end - p));
            if (!rec) break;

            const char* us = static_cast<const char*>(memchr(p, kUnitSep, rec - p));
            if (us)
            {
                NetworkChannel::RemotePlugin rp;
                rp.name.assign(p, us);
                rp.version.assign(us + 1, rec);
                if (!rp.name.empty())
                    out.push_back(std::move(rp));
            }
            p = rec + 1;
        }
        return out;
    }

    static bool HasManifest(void* conn)
    {
        if (!conn) return false;
        std::lock_guard<std::mutex> lk(g_manifestMutex);
        return g_manifests.find(conn) != g_manifests.end();
    }

    // Does this connection's reported manifest contain the plugin at that exact
    // version? False when the client never reported -- see the header note.
    static bool ClientHasPlugin(void* conn, const char* name, const char* version)
    {
        if (!conn || !name) return false;
        const char* ver = version ? version : "";

        std::lock_guard<std::mutex> lk(g_manifestMutex);
        auto it = g_manifests.find(conn);
        if (it == g_manifests.end()) return false;

        for (const auto& rp : it->second)
            if (rp.name == name && rp.version == ver)
                return true;
        return false;
    }

    // ---- Connection lookups -------------------------------------------------

    // The UNetConnection owning a remote player's controller (authority side).
    // Null for the listen host's own controller, which has no connection --
    // which is also why such a controller is never a send target.
    static void* ConnectionForPlayer(void* playerController)
    {
        if (!playerController) return nullptr;
        __try
        {
            auto* pc = reinterpret_cast<SDK::APlayerController*>(playerController);
            return pc->NetConnection;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* PlayerForConnection(void* conn)
    {
        if (!conn) return nullptr;
        __try
        {
            auto* nc = reinterpret_cast<SDK::UNetConnection*>(conn);
            return nc->PlayerController;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

#ifdef MODLOADER_CLIENT_BUILD
    // The client's connection to the server. A dedicated server has none.
    static void* ServerConnection()
    {
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world || !world->NetDriver) return nullptr;
            return world->NetDriver->ServerConnection;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
#endif // MODLOADER_CLIENT_BUILD

    // ---- Greeting (authority side) ------------------------------------------
    //
    // The one message we are willing to send to a peer we know nothing about, so
    // it has to be one that cannot hurt a peer running no loader at all. See the
    // note in modloader_hello.h for why ClientMessage is inert on a shipping
    // client, and network_channel.h for why it cannot be a control bunch.
    //
    // Retried rather than sent once, because PostLogin is early: the player
    // controller exists and is owned by the connection, but its actor channel may
    // not be open yet, and an RPC issued before it is opened goes nowhere. Rather
    // than hunt for a later trigger that is "definitely" safe -- every candidate
    // is a guess about engine state -- we send, watch for the manifest, and try
    // again. The client answering is the only proof that matters.
    //
    // Giving up is a normal outcome, not an error: it is what a vanilla client
    // looks like from here.
    //
    // A RETRY MUST NEVER RPC THE CONTROLLER THIS STATE WAS CREATED WITH.
    // The cached pointer is the one thing here that can outlive what it names, and
    // it took down a listen host in v1.19.x. The sequence:
    //
    //   00:18:19  PostLogin -> BeginGreeting, attempts = 1
    //   ...       the host travels to ChimeraMain. The game thread is inside the
    //             load for ~80 s, so OnEngineTick does not run and PumpGreetings
    //             never gets to attempt 2 -- the entry sits there, frozen at
    //             attempt 1, for the whole load. (LogNet said so plainly:
    //             "Very long time between ticks. DeltaTime: 79.94".)
    //   00:19:39  the client's connection dies waiting on that load
    //   00:19:45  RemoveClientConnection, UChannel::CleanUp, then Logout -- and
    //             Logout fires with PC->NetConnection already cleared, so
    //             OnPlayerLeftForgetConn has no key and forgets nothing
    //   00:19:46  the tick resumes, 86 s > kHelloRetryMs, attempt 2 goes out on a
    //             controller the GC has already marked unreachable ->
    //             "Assertion failed: !IsUnreachable() ... Function
    //             '/Script/Engine.PlayerController:ClientMessage' called on Object
    //             '...PersistentLevel.None' that was marked unreachable."
    //
    // Note what does NOT save this: SendHelloSEH's __except. The object was still
    // mapped and readable -- the engine's own check inside ProcessEvent is what
    // fired, and a UE fatal assert is not an SEH exception it could have caught.
    // The only fix is to not make the call.
    //
    // So the retry path resolves its target from the connection every time, and
    // only after confirming the net driver still lists that connection as live
    // (LiveControllerForConnection below). A connection the engine still owns
    // holds a hard reference to its player controller, which is precisely the
    // property "not unreachable" means -- no GC-internal flag bits needed.
    //
    // kHelloMaxAgeMs backs that up with a wall-clock deadline, because the stall
    // above is exactly the shape of failure an attempt counter cannot see: no
    // attempts were spent, so nothing aged out, while 86 seconds of engine state
    // change went past underneath.

    struct HelloState
    {
        // Diagnostics and the PlayerLeft purge only. Never an RPC target after
        // the frame it was captured on -- see the note above.
        void*    controller = nullptr;
        int      attempts   = 0;
        uint64_t lastMs     = 0;
        uint64_t startedMs  = 0;
    };

    static std::mutex                            g_helloMutex;
    static std::unordered_map<void*, HelloState> g_helloByConn; // key: UNetConnection*

    static constexpr uint64_t kHelloRetryMs     = 1000;
    static constexpr int      kHelloMaxAttempts = 10;
    static constexpr uint64_t kHelloMaxAgeMs    = 30000;

    static void ForgetConnectionState(void* conn); // defined once every store exists

    // Is this still one of the net driver's live client connections?
    //
    // Pointer comparison only -- conn is never dereferenced here, which is the
    // entire point: this is the check that decides whether dereferencing it is
    // safe. UNetDriver::RemoveClientConnection drops the entry as the peer goes
    // away (a full second before Logout, in the crash above), so falling out of
    // this list is the earliest and most reliable signal that a peer is gone.
    static bool IsLiveClientConnection(void* conn)
    {
        if (!conn) return false;
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world || !world->NetDriver) return false;

            auto* conns = &world->NetDriver->ClientConnections;
            for (int32_t i = 0; i < conns->Num(); ++i)
                if (static_cast<void*>((*conns)[i]) == conn) return true;

            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The controller it is safe to RPC for this peer right now, or null if there
    // is none. Null covers two different situations and the caller has to tell
    // them apart, which is why IsLiveClientConnection is also exposed: a live
    // connection whose controller has not resolved yet is worth waiting for, a
    // connection that has left the driver's list is not.
    static void* LiveControllerForConnection(void* conn)
    {
        if (!IsLiveClientConnection(conn)) return nullptr;
        return PlayerForConnection(conn);
    }

    // No C++ object here needs unwinding, so the SDK call can sit inside __try:
    // ProcessEvent walks engine state that a departing client can invalidate
    // between our null check and the call. This guards a torn-down object, NOT a
    // garbage-collected one -- see the note above.
    static bool SendHelloSEH(void* playerController, const wchar_t* text)
    {
        __try
        {
            auto* pc = reinterpret_cast<SDK::APlayerController*>(playerController);
            SDK::FString message(text);
            pc->ClientMessage(message, SDK::FName(0), 0.0f);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GreetClient(void* playerController)
    {
        wchar_t greeting[160] = {};
        _snwprintf_s(greeting, _TRUNCATE, L"%s%S",
                     NetworkChannel::kHelloSentinel, MODLOADER_BUILD_TAG);
        return SendHelloSEH(playerController, greeting);
    }

    // Queue a joining client for greeting. The listen host's own controller is
    // skipped: it has no connection, and it is this process.
    static bool IsLocalPlayerController(void* playerController);

    static void BeginGreeting(void* playerController)
    {
        if (!HasNetAuthority() || !playerController) return;
        if (IsLocalPlayerController(playerController)) return;

        void* conn = ConnectionForPlayer(playerController);
        if (!conn)
        {
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Player joined on controller %p with no NetConnection -- "
                L"nothing to greet", playerController);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_helloMutex);
            HelloState& st = g_helloByConn[conn];
            st.controller  = playerController;
            st.attempts    = 1;
            st.lastMs      = GetTickCount64();
            st.startedMs   = st.lastMs;
        }

        const bool ok = GreetClient(playerController);
        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Greeted peer %p (attempt 1)%s", conn,
            ok ? L"" : L" -- the RPC call itself failed");
    }

    // Drive outstanding greetings. Cheap when there are none, which is the
    // steady state for a running session.
    static void PumpGreetings(uint64_t nowMs)
    {
        // Emptiness first, authority second, and the order is not arbitrary:
        // HasNetAuthority() is a native net-mode call on client builds, and this
        // runs every frame for the whole session. A pure client leaves here
        // having taken one uncontended lock.
        {
            std::lock_guard<std::mutex> lk(g_helloMutex);
            if (g_helloByConn.empty()) return;
        }

        if (!HasNetAuthority()) return;

        std::vector<void*> toGreet;
        std::vector<void*> answered;
        std::vector<void*> gaveUp;
        // conn, attempts. Outlived kHelloMaxAgeMs without resolving.
        std::vector<std::pair<void*, int>> expired;

        {
            std::lock_guard<std::mutex> lk(g_helloMutex);
            for (auto& kv : g_helloByConn)
            {
                void*       conn = kv.first;
                HelloState& st   = kv.second;

                // Whether this peer has answered is deliberately NOT checked
                // here: that reads the manifest map, and nesting those two
                // mutexes in an order nothing else observes is how a deadlock
                // gets built. Collect candidates now, decide once the lock is out
                // of the way.
                if (nowMs - st.lastMs < kHelloRetryMs) continue;
                if (nowMs - st.startedMs >= kHelloMaxAgeMs)
                {
                    expired.push_back({ conn, st.attempts });
                    continue;
                }
                if (st.attempts >= kHelloMaxAttempts) { gaveUp.push_back(conn); continue; }
                toGreet.push_back(conn);
            }
        }

        for (void* conn : toGreet)
            if (HasManifest(conn)) answered.push_back(conn);

        for (void* conn : gaveUp)
        {
            if (HasManifest(conn)) { answered.push_back(conn); continue; }

            // A peer that has left the driver's client list did not decline to
            // answer, it disconnected -- and PlayerNameForConnection would be
            // dereferencing a dead UNetConnection to name it. Say the true thing
            // and touch nothing.
            if (!IsLiveClientConnection(conn))
            {
                ModLoaderLogger::LogDebug(
                    L"[NetworkChannel] Peer %p disconnected before answering %d greetings -- "
                    L"no conclusion drawn about it, forgetting it", conn, kHelloMaxAttempts);
                ForgetConnectionState(conn);
                continue;
            }

            // One INFO line per client, stating an outcome. The greetings
            // themselves are Debug/Trace: an operator wants to know what each
            // player turned out to be, not to watch us ask.
            std::string name = PlayerNameForConnection(conn);
            ModLoaderLogger::LogInfo(
                L"[NetworkChannel] Player '%S' is NOT running the mod loader (no answer to %d "
                L"greetings). They will be sent no plugin data. This is the expected, safe outcome "
                L"for a vanilla client -- not an error, and they stay connected.",
                name.empty() ? "(unnamed)" : name.c_str(), kHelloMaxAttempts);
        }

        for (auto& e : expired)
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Gave up greeting peer %p after %llu ms without an answer "
                L"(attempts spent: %d) -- the deadline, not the attempt count, so a long "
                L"engine stall cannot leave this pending",
                e.first, static_cast<unsigned long long>(kHelloMaxAgeMs), e.second);

        {
            std::lock_guard<std::mutex> lk(g_helloMutex);
            for (void* conn : gaveUp)   g_helloByConn.erase(conn);
            for (void* conn : answered) g_helloByConn.erase(conn);
            for (auto&  e    : expired) g_helloByConn.erase(e.first);
        }

        for (void* conn : toGreet)
        {
            if (std::find(answered.begin(), answered.end(), conn) != answered.end()) continue;

            // Resolve the target now, from the connection, and only if the driver
            // still owns that connection. Never from HelloState::controller --
            // see the note above the struct for the crash that came of it.
            void* controller = LiveControllerForConnection(conn);
            if (!controller)
            {
                if (IsLiveClientConnection(conn))
                {
                    // Connected, but no player controller yet. Wait for it rather
                    // than spending an attempt: this is a peer mid-spawn, not one
                    // ignoring us. kHelloMaxAgeMs is what stops it waiting forever.
                    std::lock_guard<std::mutex> lk(g_helloMutex);
                    auto it = g_helloByConn.find(conn);
                    if (it != g_helloByConn.end()) it->second.lastMs = nowMs;

                    ModLoaderLogger::LogTrace(
                        L"[NetworkChannel] Peer %p has no player controller yet -- greeting deferred",
                        conn);
                    continue;
                }

                // Gone from the driver's client list: the peer has disconnected
                // and anything we still hold for it is stale. Nothing is sent.
                ModLoaderLogger::LogDebug(
                    L"[NetworkChannel] Peer %p is no longer a live client connection -- "
                    L"dropping its pending greeting and forgetting it", conn);
                ForgetConnectionState(conn);
                continue;
            }

            int attempt = 0;
            {
                std::lock_guard<std::mutex> lk(g_helloMutex);
                auto it = g_helloByConn.find(conn);
                if (it == g_helloByConn.end()) continue; // peer left mid-pump
                it->second.attempts++;
                it->second.lastMs    = nowMs;
                it->second.controller = controller;
                attempt = it->second.attempts;
            }

            GreetClient(controller);
            ModLoaderLogger::LogTrace(
                L"[NetworkChannel] Re-greeted peer %p (attempt %d/%d)",
                conn, attempt, kHelloMaxAttempts);
        }
    }

    static int GreetingAttemptsFor(void* conn)
    {
        std::lock_guard<std::mutex> lk(g_helloMutex);
        auto it = g_helloByConn.find(conn);
        return it == g_helloByConn.end() ? 0 : it->second.attempts;
    }

    // ---- Wire send ----------------------------------------------------------

    // Bytes we are willing to put in one control bunch. Conservative on purpose:
    // an over-budget bunch is caught by the ArIsError check in the send helper and
    // dropped, so the cost of guessing low is a few extra frames while the cost of
    // guessing high is a lost message.
    static constexpr size_t kMaxBunchPayload = 1024;

    // Ids only need to be unique among a sender's in-flight messages, so a plain
    // counter is sufficient; wrapping after 4 billion messages is not a concern.
    static uint32_t g_nextMessageId = 1;

    // Inbound reassembly, scoped per connection so a peer that leaves takes its
    // half-finished messages with it -- a reused connection pointer must never
    // inherit a stale partial.
    static std::mutex                 g_reasmMutex;
    static Fragmentation::Reassembler g_reassembler;

    // Is this control channel still safe to send on?
    //
    // Two independent checks, because the cached UControlChannel* can outlive the
    // channel it names -- ControlChannel caches per connection and only drops an
    // entry when something tells it to, so a peer that is mid-disconnect still has
    // a cache hit right up until PlayerLeft fires.
    //
    //  1. The channel must still point back at the connection we looked it up by.
    //     This is the check that does not rely on any bit-layout assumption: if the
    //     UChannel was freed and its memory reused, Connection@0x28 almost
    //     certainly no longer matches.
    //  2. The Closing and Broken flags. Offsets come from IDA's type info for
    //     UChannel (the SDK pads this region out, so it is not in the headers):
    //     the 0x30 bitfield runs OpenAcked, Closing, Dormant, bIsReplicationPaused,
    //     OpenTemporary, Broken, ... LSB-first, giving Closing = 0x02, Broken = 0x20.
    static constexpr size_t  kOff_ChannelFlags = 0x30;
    static constexpr uint8_t kChannelClosing   = 0x02;
    static constexpr uint8_t kChannelBroken    = 0x20;

    static bool IsChannelUsable(void* cc, void* conn)
    {
        __try
        {
            auto* p = reinterpret_cast<uint8_t*>(cc);
            if (*reinterpret_cast<void**>(p + 0x28) != conn) return false;

            const uint8_t flags = *(p + kOff_ChannelFlags);
            if (flags & (kChannelClosing | kChannelBroken)) return false;

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---- Readiness (authority side) -----------------------------------------
    //
    // "Ready" is not one state, it is one per plugin: a client is a valid target
    // for plugin P version V exactly when it reported P at V, which is the same
    // predicate the send path gates on. Exposing anything coarser would invite
    // the mistake this is here to prevent -- a plugin seeing "client connected"
    // and sending to a client that has no idea what it is.
    //
    // Announcement is driven from the tick rather than from manifest receipt,
    // for one reason: a manifest can arrive before the connection has a player
    // controller, and a ready callback carrying a null controller is useless to
    // the plugin receiving it. The tick retries until the controller resolves.
    //
    // No replay buffer sits behind this, and there should never be one. The
    // loader sees opaque bytes: it cannot tell a stale position update -- worse
    // than useless when replayed three seconds late -- from state that is still
    // valid. Deciding that is the plugin's job, and this callback is what lets it.

    struct ClientReadyReg
    {
        std::string               plugin;
        std::string               version;
        PluginClientReadyCallback cb;
    };

    static std::mutex                  g_readyMutex;
    static std::vector<ClientReadyReg> g_clientReadyRegs;
    // Which callbacks have already been told about which connection.
    static std::unordered_map<void*, std::vector<PluginClientReadyCallback>> g_announced;
    static bool                        g_readyDirty = false;
    // Warn-once keys ("plugin\x01<conn>") so a mistaken send does not spam.
    static std::unordered_set<std::string> g_notReadyWarned;

    static void MarkReadyDirty()
    {
        std::lock_guard<std::mutex> lk(g_readyMutex);
        g_readyDirty = true;
    }

    // True if this (plugin, connection) pair should be warned about now.
    static bool ShouldWarnNotReady(const char* plugin, void* conn)
    {
        char key[160];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%s\x01%p", plugin ? plugin : "", conn);
        std::lock_guard<std::mutex> lk(g_readyMutex);
        return g_notReadyWarned.insert(key).second;
    }

    // Drop any pending greeting whose state names this controller.
    //
    // The connection-keyed path cannot reach these: Logout can fire with
    // PC->NetConnection already cleared, leaving nothing to look the entry up by.
    // The controller is the only handle left at that point, and it is not used as
    // an RPC target here -- only compared, which is safe on a dead pointer.
    static void ForgetHelloByController(void* playerController)
    {
        if (!playerController) return;

        std::lock_guard<std::mutex> lk(g_helloMutex);
        for (auto it = g_helloByConn.begin(); it != g_helloByConn.end(); )
        {
            if (it->second.controller == playerController)
                it = g_helloByConn.erase(it);
            else
                ++it;
        }
    }

    // Everything keyed by connection lives in five places; forgetting a peer has
    // to clear all of them or the leftovers outlive it. Defined here, after every
    // store exists.
    static void ForgetConnectionState(void* conn)
    {
        if (!conn) return;

        Hooks::ControlChannel::ForgetConnection(conn);
        {
            std::lock_guard<std::mutex> lk(g_reasmMutex);
            g_reassembler.PurgeConnection(reinterpret_cast<uintptr_t>(conn));
        }
        {
            std::lock_guard<std::mutex> lk(g_manifestMutex);
            g_manifests.erase(conn);
        }
        {
            std::lock_guard<std::mutex> lk(g_helloMutex);
            g_helloByConn.erase(conn);
        }
        {
            std::lock_guard<std::mutex> lk(g_readyMutex);
            g_announced.erase(conn);
            // Cheap and rare, and re-warning about a peer that reconnects is
            // wanted rather than avoided.
            g_notReadyWarned.clear();
        }
    }

    // Announce every (client, plugin) pair that has become ready and has not
    // been announced yet. Runs from the tick; the dirty flag keeps the steady
    // state to one lock and a bool.
    static void PumpClientReady()
    {
        // Cheap check before the native one, as in PumpGreetings.
        {
            std::lock_guard<std::mutex> lk(g_readyMutex);
            if (!g_readyDirty || g_clientReadyRegs.empty()) return;
        }

        if (!HasNetAuthority()) return;

        // Snapshot the manifests, then the registrations, then decide. Callbacks
        // are plugin code and are never invoked with a lock held.
        std::unordered_map<void*, std::vector<NetworkChannel::RemotePlugin>> manifests;
        {
            std::lock_guard<std::mutex> lk(g_manifestMutex);
            manifests = g_manifests;
        }

        struct Pending { PluginClientReadyCallback cb; void* conn; void* pc; std::string plugin; };
        std::vector<Pending> pending;
        bool stillWaiting = false;

        {
            std::lock_guard<std::mutex> lk(g_readyMutex);
            for (const auto& mkv : manifests)
            {
                void* conn = mkv.first;
                for (const auto& reg : g_clientReadyRegs)
                {
                    auto& told = g_announced[conn];
                    if (std::find(told.begin(), told.end(), reg.cb) != told.end()) continue;

                    bool match = false;
                    for (const auto& rp : mkv.second)
                        if (rp.name == reg.plugin && rp.version == reg.version) { match = true; break; }
                    if (!match) continue;

                    // The controller can lag the manifest. Leave the pair
                    // unannounced and try again next tick rather than handing a
                    // plugin a null it cannot send to.
                    void* pc = PlayerForConnection(conn);
                    if (!pc) { stillWaiting = true; continue; }

                    told.push_back(reg.cb);
                    pending.push_back({ reg.cb, conn, pc, reg.plugin });
                }
            }
            g_readyDirty = stillWaiting;
        }

        for (const auto& p : pending)
        {
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Client %p is ready for plugin '%S' -- notifying",
                p.conn, p.plugin.c_str());
            try
            {
                p.cb(p.pc);
            }
            catch (const std::exception& ex)
            {
                ModLoaderLogger::LogError(
                    L"[NetworkChannel] Exception in client-ready callback for plugin '%S': %S",
                    p.plugin.c_str(), ex.what());
            }
            catch (...)
            {
                ModLoaderLogger::LogError(
                    L"[NetworkChannel] Unknown exception in client-ready callback for plugin '%S'",
                    p.plugin.c_str());
            }
        }
    }

    // ---- Periodic housekeeping ----------------------------------------------

    // Reassembly buffers are reclaimed by three mechanisms, and all three are
    // needed:
    //
    //   1. PlayerLeft, when the departing controller still names its connection.
    //   2. The send path, which forgets a peer the moment its channel reads as
    //      closing, broken or recycled.
    //   3. This sweep -- the only one that does not depend on being told.
    //
    // (3) exists because a client can vanish mid-transfer in ways (1) and (2)
    // never observe: Logout can fire after the connection pointer is already
    // gone, and nothing may ever try to send to that peer again. ExpireStale is
    // otherwise only reached from Feed, so on an otherwise idle authority a
    // half-received message from a departed client would sit there until the
    // world tore down.
    static constexpr uint64_t kSweepIntervalMs = 1000;
    static uint64_t g_lastSweepMs = 0;

    static void SweepStalePartials(uint64_t nowMs)
    {
        if (g_lastSweepMs && (nowMs - g_lastSweepMs) < kSweepIntervalMs) return;
        g_lastSweepMs = nowMs;

        size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lk(g_reasmMutex);
            if (g_reassembler.PartialCount() == 0) return;
            dropped = g_reassembler.ExpireStale(nowMs);
        }

        if (dropped)
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Swept %zu abandoned partial message(s) -- a sender "
                L"disconnected or stopped mid-transfer", dropped);
    }


    // One bunch, no chunking. Every send ultimately funnels through here.
    static bool SendBunch(void* conn, const uint8_t* bytes, size_t len)
    {
        void* cc = Hooks::ControlChannel::GetControlChannel(conn);
        if (!cc)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Wire send dropped: no control channel open for peer %p", conn);
            return false;
        }

        if (!IsChannelUsable(cc, conn))
        {
            // Disconnecting, torn down, or a recycled allocation. Forget the peer
            // now: continuing to hold its reassembly buffers and manifest would
            // keep a departing client alive in our state until something else
            // noticed, and the cached channel would be retried on every send.
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Wire send dropped: control channel for peer %p is closing, "
                L"broken or stale -- forgetting the peer", conn);
            ForgetConnectionState(conn);
            return false;
        }

        const bool ok = Hooks::ControlChannel::SendRaw(cc, bytes, len);

        if (!ok)
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Wire send failed for peer %p (%zu bytes)", conn, len);
        else
            ModLoaderLogger::LogTrace(
                L"[NetworkChannel] Wire send ok: peer=%p %zu bytes", conn, len);

        return ok;
    }

    // Put an envelope on the wire, chunking it if it will not fit in one bunch.
    // There is no fallback -- a false return means the message was dropped, and
    // the log line says why.
    //
    // label is carried in each chunk header purely for diagnostics: a partial
    // message that never completes is otherwise an anonymous stuck buffer.
    static bool SendWire(void* conn, const char* label, const std::string& env)
    {
        if (!conn)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Wire send dropped: no NetConnection for target");
            return false;
        }

        if (!Hooks::ControlChannel::IsAvailable())
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Wire send dropped: control channel unavailable "
                L"(one or more patterns failed to resolve -- see preflight warnings)");
            return false;
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(env.data());

        if (env.size() <= kMaxBunchPayload)
            return SendBunch(conn, bytes, env.size());

        const std::string tag = label ? label : "";
        if (Fragmentation::kFragHeaderBase + tag.size() >= kMaxBunchPayload)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Cannot fragment: label '%S' leaves no room for payload", tag.c_str());
            return false;
        }
        const size_t budget = kMaxBunchPayload - Fragmentation::kFragHeaderBase - tag.size();

        const uint32_t msgId = g_nextMessageId++;
        auto frames = Fragmentation::BuildChunks(msgId, tag, bytes, env.size(), budget);

        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Fragmenting %zu bytes into %zu chunk(s) of <=%zu "
            L"(message %u, label '%S') for peer %p",
            env.size(), frames.size(), budget, msgId, tag.c_str(), conn);

        // All-or-nothing: a partially sent message leaves the peer holding a
        // buffer that can only ever expire, so report failure the moment a chunk
        // does not go out rather than pretending the message was delivered.
        for (size_t i = 0; i < frames.size(); ++i)
        {
            if (!SendBunch(conn, frames[i].data(), frames[i].size()))
            {
                ModLoaderLogger::LogError(
                    L"[NetworkChannel] Fragment %zu/%zu of message %u failed -- "
                    L"peer %p will never complete it",
                    i + 1, frames.size(), msgId, conn);
                return false;
            }
        }

        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] Sent all %zu chunk(s) of message %u to peer %p",
            frames.size(), msgId, conn);
        return true;
    }

// ============================================================
// Authority side -- compiled on both server and client builds.
// Every entry point here is gated on HasNetAuthority() at runtime.
// ============================================================

    // Send one envelope to a player. Wire-only: the target's UNetConnection is
    // resolved from its controller, which is also why the listen host's own
    // controller can never be a target -- it has no connection.
    static void SendEnvelopeToPlayer(void* playerController, const char* label,
                                     const std::string& envelope)
    {
        SendWire(ConnectionForPlayer(playerController), label, envelope);
    }


    // Server-side handler registry for Client->Server messages
    static std::mutex g_serverMutex;
    static std::unordered_map<std::string, std::vector<PluginNetworkServerMessageCallback>> g_serverHandlers;

    // IPluginNetworkChannel function implementations -- authority side

    static bool IsExcluded(void* playerController);

    // On a listen host, GetAllActorsOfClass also returns the host's OWN player
    // controller, which has no UNetConnection -- there is no wire to itself. The
    // send would fail anyway; this catches it early and says so precisely instead
    // of logging a dropped-send warning on every broadcast.
    //
    // So "all clients" means all *remote* clients, on a listen host exactly as on a
    // dedicated server.  Host-side code already holds the authoritative data in
    // process and can call its own handler directly; it does not need the wire.
    static bool IsLocalPlayerController(void* playerController)
    {
#ifdef MODLOADER_CLIENT_BUILD
        if (!playerController) return false;

        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world) return false;

        void* localPC = SDK::UGameplayStatics::GetPlayerController(world, 0);
        return localPC && localPC == playerController;
#else
        (void)playerController;
        return false; // a dedicated server has no local player controller
#endif
    }

    static void NC_SendPacketToClient(void* playerController, const IPluginSelf* self,
                                      const char* typeTag, const uint8_t* data, size_t size)
    {
        if (!playerController || !self || !self->name || !typeTag || !data || size == 0) return;

        if (!HasNetAuthority())
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] SendPacketToClient ignored for plugin '%S': not the session authority "
                L"(only a dedicated server or a listen host can send to clients)",
                self->name);
            return;
        }

        if (IsLocalPlayerController(playerController))
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] SendPacketToClient ignored for plugin '%S': target is the listen host's "
                L"own controller, which has no connection to send over -- "
                L"call the handler directly instead",
                self->name);
            return;
        }

        // Only send to a client that reported this exact plugin and version.
        // A version mismatch is treated as absence on purpose: two builds of a
        // plugin that disagree about their own packet layout is precisely the
        // case this exists to stop.
        void* targetConn = ConnectionForPlayer(playerController);
        if (!ClientHasPlugin(targetConn, self->name, self->version))
        {
            // Warned, not logged at Debug, and warned once per plugin per peer:
            // a targeted send names a specific recipient, so a dropped one is
            // nearly always a plugin sending at player-join instead of waiting
            // for its ready callback -- which is invisible at Debug and looks
            // like the network losing packets. Broadcasts stay at Trace: they
            // skip non-matching clients by design and would drown this out.
            if (ShouldWarnNotReady(self->name, targetConn))
            {
                ModLoaderLogger::LogWarn(
                    L"[NetworkChannel] SendPacketToClient DROPPED for plugin '%S': player=%p has not "
                    L"reported '%S' version '%S' (greeting attempts so far: %d). If you are sending "
                    L"from a player-joined hook, that is too early -- use "
                    L"Network->RegisterClientReadyCallback. Further drops for this pair are silent.",
                    self->name, playerController, self->name,
                    self->version ? self->version : "", GreetingAttemptsFor(targetConn));
            }
            else
            {
                ModLoaderLogger::LogTrace(
                    L"[NetworkChannel] SendPacketToClient skipped: player=%p not ready for '%S'",
                    playerController, self->name);
            }
            return;
        }

        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] SendPacketToClient: player=%p plugin='%S' tag='%S' payload=%zu bytes",
            playerController,
            self->name,
            typeTag,
            size);

        if (size > 1400)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Payload %zu bytes exceeds 1400-byte recommended limit for plugin '%S'",
                size, self->name);
        }

        std::string env = BuildEnvelope(self->name, typeTag, data, size);
        SendEnvelopeToPlayer(playerController, self->name, env);
    }

    static void NC_SendPacketToAllClients(const IPluginSelf* self, const char* typeTag,
                                          const uint8_t* data, size_t size)
    {
        if (!self || !self->name || !typeTag || !data || size == 0) return;

        if (!HasNetAuthority())
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] SendPacketToAllClients ignored for plugin '%S': not the session authority "
                L"(only a dedicated server or a listen host can broadcast)",
                self->name);
            return;
        }

        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] SendPacketToAllClients: plugin='%S' tag='%S' payload=%zu bytes",
            self->name,
            typeTag,
            size);

        if (size > 1400)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Payload %zu bytes exceeds 1400-byte recommended limit for plugin '%S'",
                size, self->name);
        }

        std::string env = BuildEnvelope(self->name, typeTag, data, size);

        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
        {
            ModLoaderLogger::LogWarn(L"[NetworkChannel] SendPacketToAllClients: UWorld not available");
            return;
        }

        SDK::TArray<SDK::AActor*> actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(
            world,
            SDK::ACrPlayerControllerBase::StaticClass(),
            &actors);

        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] SendPacketToAllClients: found %d player controllers",
            actors.Num());

        int32_t sent = 0;
        for (int32_t i = 0; i < actors.Num(); ++i)
        {
	        if (IsExcluded(actors[i]))
	        {
	            ModLoaderLogger::LogTrace(
	                L"[NetworkChannel] SendPacketToAllClients: skipping excluded controller %p",
	                actors[i]);
	            continue;
	        }
	        if (IsLocalPlayerController(actors[i]))
	        {
	            ModLoaderLogger::LogTrace(
	                L"[NetworkChannel] SendPacketToAllClients: skipping listen-host local controller %p",
	                actors[i]);
	            continue;
	        }
	        if (!ClientHasPlugin(ConnectionForPlayer(actors[i]), self->name, self->version))
	        {
	            ModLoaderLogger::LogTrace(
	                L"[NetworkChannel] SendPacketToAllClients: skipping %p -- no matching '%S' %S",
	                actors[i], self->name, self->version ? self->version : "");
	            continue;
	        }
	        ++sent;
	        SendEnvelopeToPlayer(actors[i], self->name, env);
        }

        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] SendPacketToAllClients: delivered to %d of %d controllers for plugin '%S'",
            sent, actors.Num(), self->name);
    }

#ifndef MODLOADER_CLIENT_BUILD
    // Server->Client receive and Client->Server send exist only on client builds.
    // A dedicated server never receives a ClientMessage and never sends to itself.
    // (A listen host uses the real client-build implementations further down --
    // its loopback to itself goes through the same two RPCs, executed locally.)

    static void NC_RegisterMessageHandler(const IPluginSelf*, const char*, PluginNetworkMessageCallback)
    {
        // No-op on server -- server only sends to clients, never receives ClientMessage
    }

    static void NC_UnregisterMessageHandler(const IPluginSelf*, const char*, PluginNetworkMessageCallback)
    {
        // No-op on server
    }

    // Client->Server: no-op on server side (server only receives)
    static void NC_SendPacketToServer(const IPluginSelf*, const char*, const uint8_t*, size_t)
    {
        // No-op on server
    }

    // A dedicated server has no server to become ready to.
    static bool NC_IsServerReady() { return false; }

    static void NC_RegisterServerReadyCallback(const IPluginSelf* self, PluginServerReadyCallback)
    {
        if (self && self->name)
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] RegisterServerReadyCallback ignored for plugin '%S': a dedicated "
                L"server is the authority and has no server to connect to",
                self->name);
    }

    static void NC_UnregisterServerReadyCallback(const IPluginSelf*, PluginServerReadyCallback) {}
#endif // !MODLOADER_CLIENT_BUILD

    static void NC_RegisterServerMessageHandler(const IPluginSelf* self, const char* typeTag,
                                                PluginNetworkServerMessageCallback callback)
    {
        if (!self || !self->name || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(self->name, typeTag);
        {
            std::lock_guard<std::mutex> lk(g_serverMutex);
            g_serverHandlers[key].push_back(callback);
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Server handler count for plugin='%S' tag='%S' is now %zu",
                self->name,
                typeTag,
                g_serverHandlers[key].size());
        }
        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Server handler registered for plugin='%S' tag='%S'",
            self->name, typeTag);
    }

    static void NC_UnregisterServerMessageHandler(const IPluginSelf* self, const char* typeTag,
                                                  PluginNetworkServerMessageCallback callback)
    {
        if (!self || !self->name || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(self->name, typeTag);
        std::lock_guard<std::mutex> lk(g_serverMutex);
        auto it = g_serverHandlers.find(key);
        if (it != g_serverHandlers.end())
        {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), callback), vec.end());
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Server handler count for plugin='%S' tag='%S' after unregister is %zu",
                self->name,
                typeTag,
                vec.size());
            if (vec.empty())
                g_serverHandlers.erase(it);
        }
    }

    // Broadcast exclusion list -- controllers registered here are skipped by SendPacketToAllClients.
    static std::mutex g_excludeMutex;
    static std::vector<void*> g_excludedControllers;

    static void NC_ExcludeFromBroadcast(void* playerController)
    {
        if (!playerController) return;
        std::lock_guard<std::mutex> lk(g_excludeMutex);
        for (auto* p : g_excludedControllers)
            if (p == playerController) return; // already registered
        g_excludedControllers.push_back(playerController);
        ModLoaderLogger::LogDebug(L"[NetworkChannel] ExcludeFromBroadcast: %p registered", playerController);
    }

    static void NC_UnexcludeFromBroadcast(void* playerController)
    {
        if (!playerController) return;
        std::lock_guard<std::mutex> lk(g_excludeMutex);
        g_excludedControllers.erase(
            std::remove(g_excludedControllers.begin(), g_excludedControllers.end(), playerController),
            g_excludedControllers.end());
        ModLoaderLogger::LogDebug(L"[NetworkChannel] UnexcludeFromBroadcast: %p removed", playerController);
    }

    static bool IsExcluded(void* playerController)
    {
        std::lock_guard<std::mutex> lk(g_excludeMutex);
        for (auto* p : g_excludedControllers)
            if (p == playerController) return true;
        return false;
    }

    // ---- Readiness API (authority side) -------------------------------------

    static bool NC_IsClientReady(void* playerController, const IPluginSelf* self)
    {
        if (!playerController || !self || !self->name) return false;
        if (!HasNetAuthority()) return false;
        if (IsLocalPlayerController(playerController)) return false;
        return ClientHasPlugin(ConnectionForPlayer(playerController), self->name, self->version);
    }

    static void NC_RegisterClientReadyCallback(const IPluginSelf* self, PluginClientReadyCallback callback)
    {
        if (!self || !self->name || !callback) return;

        if (!HasNetAuthority())
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] RegisterClientReadyCallback ignored for plugin '%S': this process "
                L"is not the session authority, so it has no clients to become ready",
                self->name);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_readyMutex);
            for (const auto& reg : g_clientReadyRegs)
                if (reg.cb == callback && reg.plugin == self->name) return; // already registered

            g_clientReadyRegs.push_back(
                { self->name, self->version ? self->version : "", callback });

            // Clients already connected and already ready are announced on the
            // next tick. This is what makes registering from a reloaded plugin
            // work: without it a reload mid-session would hear about nobody.
            g_readyDirty = true;
        }

        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Client-ready callback registered for plugin='%S' version='%S'",
            self->name, self->version ? self->version : "");
    }

    static void NC_UnregisterClientReadyCallback(const IPluginSelf* self, PluginClientReadyCallback callback)
    {
        if (!self || !self->name || !callback) return;

        std::lock_guard<std::mutex> lk(g_readyMutex);
        g_clientReadyRegs.erase(
            std::remove_if(g_clientReadyRegs.begin(), g_clientReadyRegs.end(),
                           [&](const ClientReadyReg& r)
                           { return r.cb == callback && r.plugin == self->name; }),
            g_clientReadyRegs.end());

        // Drop the "already told" records too, so re-registering later announces
        // from scratch rather than silently skipping every current client.
        for (auto& kv : g_announced)
            kv.second.erase(std::remove(kv.second.begin(), kv.second.end(), callback),
                            kv.second.end());
    }

// ============================================================
// End of authority side
// ============================================================

#ifdef MODLOADER_CLIENT_BUILD

    static std::mutex g_mutex;

    // Handler registry for Server->Client messages: key = "pluginName\x01typeTag"
    static std::unordered_map<std::string, std::vector<PluginNetworkMessageCallback>> g_handlers;

    static void NC_RegisterMessageHandler(const IPluginSelf* self, const char* typeTag,
                                          PluginNetworkMessageCallback callback)
    {
        if (!self || !self->name || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(self->name, typeTag);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_handlers[key].push_back(callback);
            ModLoaderLogger::LogTrace(
                L"[NetworkChannel] Client handler count for plugin='%S' tag='%S' is now %zu",
                self->name,
                typeTag,
                g_handlers[key].size());
        }
        ModLoaderLogger::LogTrace(L"[NetworkChannel] Handler registered for plugin='%S' tag='%S'",
                                  self->name, typeTag);
        // No lazy hook install here any more: the receive path is the control
        // channel detour, installed once by Initialize().
    }

    static void NC_UnregisterMessageHandler(const IPluginSelf* self, const char* typeTag,
                                            PluginNetworkMessageCallback callback)
    {
        if (!self || !self->name || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(self->name, typeTag);
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_handlers.find(key);
        if (it != g_handlers.end())
        {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), callback), vec.end());
            if (vec.empty())
                g_handlers.erase(it);
        }
    }

    static void SendManifestToServer(bool force);

    // ---- The gate -----------------------------------------------------------
    //
    // Nothing this client sends reaches the control channel until the authority
    // has greeted us. This flag, not the greeting itself, is the fix for the
    // disconnect: an ungreeted client that stays silent survives on a server
    // running no loader at all, which is what joining one should look like.
    //
    // Cleared on world end play, because the next world is a different
    // connection and says nothing about whether that peer speaks the wire.
    static bool        g_serverSpeaksWire = false;
    static std::string g_serverGreetTag;

    // Not being greeted is silent by design -- but silence is exactly what a
    // player reports as "the mod does nothing on that server", so it gets one
    // INFO line saying so. Timed from world begin play rather than fired on any
    // event, because the case being described is one where no event arrives.
    //
    // 20s: the authority greets at PostLogin and retries for ~10s, so anything
    // past that is a settled answer rather than a slow join.
    static constexpr uint64_t kNoGreetingNoticeMs = 20000;
    static uint64_t g_worldStartedMs   = 0;
    static bool     g_noGreetingLogged = false;

    // Plugins that tried to send before we were greeted, so the warning fires
    // once each rather than once per packet.
    static std::unordered_set<std::string> g_earlySendWarned;

    // ---- Server-ready callbacks (client side) -------------------------------
    static std::mutex                              g_srvReadyMutex;
    static std::vector<PluginServerReadyCallback>  g_serverReadyCbs;
    static std::vector<PluginServerReadyCallback>  g_serverReadyFired;

    static bool ServerReadyNow(); // defined below, next to the ack handler

    static void FireServerReady(const std::vector<PluginServerReadyCallback>& cbs,
                                const std::string& tag)
    {
        for (auto* cb : cbs)
        {
            if (!cb) continue;
            try { cb(tag.c_str()); }
            catch (const std::exception& ex)
            {
                ModLoaderLogger::LogError(
                    L"[NetworkChannel] Exception in server-ready callback: %S", ex.what());
            }
            catch (...)
            {
                ModLoaderLogger::LogError(L"[NetworkChannel] Unknown exception in server-ready callback");
            }
        }
    }

    static void NC_RegisterServerReadyCallback(const IPluginSelf* self, PluginServerReadyCallback callback)
    {
        if (!self || !self->name || !callback) return;

        std::vector<PluginServerReadyCallback> fireNow;
        std::string tag;
        {
            std::lock_guard<std::mutex> lk(g_srvReadyMutex);
            for (auto* cb : g_serverReadyCbs)
                if (cb == callback) return; // already registered
            g_serverReadyCbs.push_back(callback);

            // Registering after the fact must not lose the event -- a plugin
            // loaded or reloaded mid-session would otherwise wait forever for a
            // readiness it had already missed.
            if (ServerReadyNow())
            {
                g_serverReadyFired.push_back(callback);
                fireNow.push_back(callback);
                tag = g_serverGreetTag;
            }
        }

        if (!fireNow.empty())
        {
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Server already ready -- firing callback immediately for plugin '%S'",
                self->name);
            FireServerReady(fireNow, tag);
        }
    }

    static void NC_UnregisterServerReadyCallback(const IPluginSelf* self, PluginServerReadyCallback callback)
    {
        if (!self || !callback) return;
        std::lock_guard<std::mutex> lk(g_srvReadyMutex);
        g_serverReadyCbs.erase(std::remove(g_serverReadyCbs.begin(), g_serverReadyCbs.end(), callback),
                               g_serverReadyCbs.end());
        g_serverReadyFired.erase(std::remove(g_serverReadyFired.begin(), g_serverReadyFired.end(), callback),
                                 g_serverReadyFired.end());
    }

    static void NC_SendPacketToServer(const IPluginSelf* self, const char* typeTag,
                                      const uint8_t* data, size_t size)
    {
        if (!self || !self->name || !typeTag || !data || size == 0) return;

        if (!g_serverSpeaksWire)
        {
            bool first = false;
            {
                std::lock_guard<std::mutex> lk(g_srvReadyMutex);
                first = g_earlySendWarned.insert(self->name).second;
            }
            if (first)
                ModLoaderLogger::LogWarn(
                    L"[NetworkChannel] SendPacketToServer DROPPED for plugin '%S': the server has not "
                    L"identified itself as running the mod loader. Either it is not, or we have not "
                    L"been greeted yet -- use Network->RegisterServerReadyCallback rather than sending "
                    L"on join. Further drops for this plugin are silent.",
                    self->name);
            return;
        }

        // Insurance against the world-begin-play report having failed (channel
        // not open yet, say). A no-op once it has succeeded for this connection.
        // Ordering holds: control bunches are reliable and in-order, so a manifest
        // queued here still arrives ahead of the packet below it.
        SendManifestToServer(/*force*/ false);

        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] SendPacketToServer: plugin='%S' tag='%S' payload=%zu bytes",
            self->name,
            typeTag,
            size);

        if (size > 1400)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] SendPacketToServer: payload %zu bytes exceeds 1400-byte limit for plugin '%S'",
                size, self->name);
        }

        SendWire(ServerConnection(), self->name, BuildEnvelope(self->name, typeTag, data, size));
    }

    // Tell the authority which plugins we have.
    //
    // Keyed on BOTH the connection and the plugin generation, because the report
    // describes a plugin set, not a session. Hot-reloading a plugin mid-session
    // changes what we have without changing who we are connected to, and the
    // failures from missing that are silent ones: a newly loaded plugin the
    // server never learns about receives nothing forever, and a reloaded plugin
    // whose version string moved gets its traffic gated off. GetPluginGeneration()
    // increments on every load/unload/reload, which is exactly this question.
    static void*    g_manifestSentTo  = nullptr;
    static unsigned g_manifestSentGen = 0;

    // An unacknowledged manifest is retried rather than assumed delivered. This
    // is deliberately how the "when is the connection actually ready?" problem is
    // handled: any trigger we pick is a guess about engine state, so instead of
    // guessing well we send, wait for the authority to answer, and try again if
    // it does not. WorldBeginPlay is then just the first attempt, not the only
    // chance -- and a persistent failure shows up as a log line rather than a
    // plugin that mysteriously never receives anything.
    static bool     g_manifestAcked   = false;
    static uint64_t g_lastAttemptMs   = 0;
    static int      g_attempts        = 0;
    static bool     g_gaveUpLogged    = false;

    static constexpr uint64_t kRetryIntervalMs = 2000;
    static constexpr int      kMaxAttempts     = 10;

    static void SendManifestToServer(bool force)
    {
        // A listen host is its own authority; it has no server to report to, and
        // its own plugin set is already the reference the gating compares against.
        if (HasNetAuthority()) return;

        // THE gate. Reporting our plugins is the first thing this client would
        // ever put on the control channel, and doing that to a server running no
        // loader is what closed the connection. No greeting, no wire.
        if (!g_serverSpeaksWire) return;

        void* conn = ServerConnection();
        if (!conn) return;

        const unsigned gen = PluginManager::GetPluginGeneration();

        // A new connection, or a changed plugin set, invalidates any previous
        // acknowledgement -- what the authority agreed to is no longer what we have.
        if (conn != g_manifestSentTo || gen != g_manifestSentGen)
        {
            if (g_manifestSentTo)
                ModLoaderLogger::LogDebug(
                    L"[NetworkChannel] Manifest invalidated (conn %p->%p, generation %u->%u) -- resending",
                    g_manifestSentTo, conn, g_manifestSentGen, gen);

            g_manifestSentTo  = conn;
            g_manifestSentGen = gen;
            g_manifestAcked   = false;
            g_attempts        = 0;
            g_gaveUpLogged    = false;
            g_lastAttemptMs   = 0;
        }

        if (g_manifestAcked && !force) return;

        const uint64_t now = GetTickCount64();
        if (!force)
        {
            if (g_attempts >= kMaxAttempts)
            {
                if (!g_gaveUpLogged)
                {
                    g_gaveUpLogged = true;
                    // "The server probably has no mod loader" is no longer a
                    // possible explanation: we only send this after the server
                    // greeted us, so it has one. Something else is wrong, and
                    // saying the old thing would send whoever reads this after
                    // the wrong problem entirely.
                    ModLoaderLogger::LogError(
                        L"[NetworkChannel] Gave up reporting our plugin manifest after %d attempts. "
                        L"The server greeted us, so it IS running the mod loader -- but it never "
                        L"acknowledged our reply. This client will receive NO plugin packets. Check "
                        L"the server's log for the other end of this exchange.",
                        kMaxAttempts);
                }
                return;
            }
            if (g_lastAttemptMs && (now - g_lastAttemptMs) < kRetryIntervalMs)
                return;
        }

        std::string body = EncodeManifest();
        const int count  = PluginManager::GetLoadedPluginCount();

        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Sending plugin manifest: %d plugin(s), %zu bytes, generation %u, attempt %d/%d",
            count, body.size(), gen, g_attempts + 1, kMaxAttempts);

        g_lastAttemptMs = now;
        ++g_attempts;

        if (SendWire(conn, kMlPlugin, BuildEnvelope(kMlPlugin, kMlManifest,
                                         reinterpret_cast<const uint8_t*>(body.data()),
                                         body.size())))
        {
            // Only the first attempt is INFO. A retry means the previous one was
            // not acknowledged, which the give-up ERROR below reports properly --
            // ten INFO lines saying the same thing would just bury it.
            if (g_attempts == 1)
                ModLoaderLogger::LogInfo(
                    L"[NetworkChannel] Reported our %d plugin(s) to the server -- awaiting "
                    L"acknowledgement", count);
            else
                ModLoaderLogger::LogDebug(
                    L"[NetworkChannel] Re-reported %d plugin(s) (generation %u, attempt %d/%d)",
                    count, gen, g_attempts, kMaxAttempts);
        }
        else
        {
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Manifest attempt %d could not be put on the wire; will retry in %llums",
                g_attempts, kRetryIntervalMs);
        }
    }

    // The authority greeted us. Everything the client can do on the wire starts
    // here, and until it happens this client is indistinguishable from one with
    // no loader at all -- deliberately, because that is what makes it safe on a
    // server that has none.
    static void OnServerGreeting(const wchar_t* payload)
    {
        char narrow[128] = {};
        if (payload && *payload)
            WideCharToMultiByte(CP_UTF8, 0, payload, -1, narrow, sizeof(narrow), nullptr, nullptr);

        if (g_serverSpeaksWire)
        {
            // The authority retries until we answer, so duplicates are normal
            // rather than a fault -- our manifest and its greeting crossed.
            ModLoaderLogger::LogTrace(
                L"[NetworkChannel] Repeat greeting from the authority (build '%S')", narrow);
            return;
        }

        g_serverSpeaksWire = true;
        g_serverGreetTag   = narrow[0] ? narrow : "(unknown)";

        ModLoaderLogger::LogInfo(
            L"[NetworkChannel] The server IS running the mod loader (build '%S') -- reporting our "
            L"plugins to it now. Nothing was sent before this point.", g_serverGreetTag.c_str());

        SendManifestToServer(/*force*/ true);
    }

    // Say once, out loud, that this session has no plugin networking. Runs from
    // the tick on the not-greeted path, so it must stay cheap: a comparison of
    // two integers until the moment it fires, and one net-driver read after.
    static void ReportNoGreetingOnce()
    {
        if (g_noGreetingLogged || g_worldStartedMs == 0) return;
        if (GetTickCount64() - g_worldStartedMs < kNoGreetingNoticeMs) return;

        g_noGreetingLogged = true; // one shot per world, whatever we conclude

        // Single player and listen hosts have no server connection and are not
        // waiting to be greeted by anyone. Nothing to report.
        if (!ServerConnection()) return;

        ModLoaderLogger::LogInfo(
            L"[NetworkChannel] The server has not identified itself as running the mod loader, so "
            L"plugin networking is INACTIVE for this session.");
    }

    // Manifest refresh keyed on the plugin generation. The alternative --
    // notifying from every load/unload/reload site -- is the arrangement
    // CLAUDE.md warns about: it misses whichever site nobody remembered to
    // update, and the symptom is a plugin that silently receives nothing.
    static void RefreshManifestIfStale()
    {
        // Steady state is one bool and one integer compare. Everything past this
        // point touches the world, the net driver and a native net-mode call --
        // fine occasionally, not fine on every frame of a running game for the
        // entire session. A connection change without a plugin change is caught
        // by the world begin/end play handlers, which clear the acked flag.
        //
        // The greeting check has to be part of that early-out, not left to the
        // gate inside SendManifestToServer: an ungreeted client is the ordinary
        // case on a server without the loader, and it lasts the whole session,
        // so falling through to the net-mode call every frame is precisely the
        // cost this early-out exists to avoid.
        if (!g_serverSpeaksWire) { ReportNoGreetingOnce(); return; }
        if (g_manifestAcked && PluginManager::GetPluginGeneration() == g_manifestSentGen)
            return;

        SendManifestToServer(/*force*/ false);
    }

    // The authority answered our manifest. Payload is buildTag US interfaceVersion RS.
    static void OnManifestAck(const uint8_t* data, size_t len)
    {
        std::string tag  = "(unknown)";
        std::string iface = "?";

        const char* p   = reinterpret_cast<const char*>(data);
        const char* rec = static_cast<const char*>(memchr(p, kRecordSep, len));
        if (rec)
        {
            const char* us = static_cast<const char*>(memchr(p, kUnitSep, rec - p));
            if (us)
            {
                tag.assign(p, us);
                iface.assign(us + 1, rec);
            }
        }

        const bool first = !g_manifestAcked;
        g_manifestAcked  = true;

        if (first)
            ModLoaderLogger::LogInfo(
                L"[NetworkChannel] Server acknowledged our manifest after %d attempt(s) "
                L"-- mod loader '%S', plugin interface v%S. Plugin packets will now flow.",
                g_attempts, tag.c_str(), iface.c_str());
        else
            ModLoaderLogger::LogDebug(
                L"[NetworkChannel] Duplicate manifest acknowledgement from server (build '%S')",
                tag.c_str());

        // The ack, not the greeting, is what plugins are told about: it is the
        // first moment the authority is known to have our plugin list, and
        // therefore the first moment a packet from us can be routed anywhere.
        std::vector<PluginServerReadyCallback> toFire;
        std::string readyTag;
        {
            std::lock_guard<std::mutex> lk(g_srvReadyMutex);
            readyTag = g_serverGreetTag.empty() ? tag : g_serverGreetTag;
            for (auto* cb : g_serverReadyCbs)
            {
                if (std::find(g_serverReadyFired.begin(), g_serverReadyFired.end(), cb)
                    != g_serverReadyFired.end())
                    continue;
                g_serverReadyFired.push_back(cb);
                toFire.push_back(cb);
            }
        }
        FireServerReady(toFire, readyTag);
    }

    static bool ServerReadyNow() { return !HasNetAuthority() && g_manifestAcked; }

    static bool NC_IsServerReady() { return ServerReadyNow(); }

    // ---- Echo self-test (client side) ---------------------------------------

    static size_t   g_echoExpectedBytes = 0;
    static uint64_t g_echoStartedMs     = 0;
    static bool     g_echoInFlight      = false;

    static bool StartEchoTest(size_t bytes, std::string& err)
    {
        if (HasNetAuthority())
        {
            err = "This is the authority -- there is no server to echo off. "
                  "Run the test from a connected client.";
            return false;
        }
        if (!Hooks::ControlChannel::IsAvailable())
        {
            err = "Control channel unavailable; see preflight warnings.";
            return false;
        }
        if (!g_serverSpeaksWire)
        {
            err = "The server has not greeted us, so it is either not running the mod loader or has "
                  "not got to us yet. Sending anyway would disconnect a vanilla server.";
            return false;
        }
        void* conn = ServerConnection();
        if (!conn)
        {
            err = "Not connected to a server.";
            return false;
        }
        if (bytes == 0 || bytes > 4u * 1024u * 1024u)
        {
            err = "Size must be between 1 and 4194304 bytes.";
            return false;
        }

        std::vector<uint8_t> payload(bytes);
        for (size_t i = 0; i < bytes; ++i)
            payload[i] = EchoByteAt(i);

        g_echoExpectedBytes = bytes;
        g_echoStartedMs     = GetTickCount64();
        g_echoInFlight      = true;

        ModLoaderLogger::LogInfo(
            L"[NetworkChannel] Echo test: sending %zu bytes (bunch budget %zu, so this "
            L"%S be fragmented)",
            bytes, kMaxBunchPayload,
            bytes > kMaxBunchPayload ? L"WILL" : L"will NOT");

        if (!SendWire(conn, kMlPlugin,
                      BuildEnvelope(kMlPlugin, kMlEcho, payload.data(), payload.size())))
        {
            g_echoInFlight = false;
            err = "Send failed; see modloader.log.";
            return false;
        }
        return true;
    }

    static void OnEchoReply(const uint8_t* data, size_t len)
    {
        if (!g_echoInFlight)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Echo test: unexpected reply (%zu bytes), no test running", len);
            return;
        }
        g_echoInFlight = false;

        const uint64_t elapsed = GetTickCount64() - g_echoStartedMs;

        if (len != g_echoExpectedBytes)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] ECHO TEST FAILED: sent %zu bytes, got %zu back (%llums)",
                g_echoExpectedBytes, len, elapsed);
            return;
        }

        for (size_t i = 0; i < len; ++i)
        {
            if (data[i] != EchoByteAt(i))
            {
                ModLoaderLogger::LogError(
                    L"[NetworkChannel] ECHO TEST FAILED: %zu bytes returned but byte %zu is "
                    L"0x%02X, expected 0x%02X. Length was right and content was not, which "
                    L"points at chunk ordering or offsets rather than loss.",
                    len, i, data[i], EchoByteAt(i));
                return;
            }
        }

        ModLoaderLogger::LogInfo(
            L"[NetworkChannel] ECHO TEST PASSED: %zu bytes round-tripped byte-for-byte in %llums",
            len, elapsed);
    }

#endif // MODLOADER_CLIENT_BUILD

    // Static IPluginNetworkChannel instance (shared between server and client)
    static IPluginNetworkChannel g_networkIface =
    {
        NC_IsServer,
        NC_SendPacketToClient,
        NC_SendPacketToAllClients,
        NC_RegisterMessageHandler,
        NC_UnregisterMessageHandler,
        NC_SendPacketToServer,
        NC_RegisterServerMessageHandler,
        NC_UnregisterServerMessageHandler,
        NC_ExcludeFromBroadcast,
        NC_UnexcludeFromBroadcast,
        // v56 -- appended, never inserted: existing plugins read every field
        // above this line by offset.
        NC_IsClientReady,
        NC_RegisterClientReadyCallback,
        NC_UnregisterClientReadyCallback,
        NC_IsServerReady,
        NC_RegisterServerReadyCallback,
        NC_UnregisterServerReadyCallback,
    };

} // anonymous namespace

// ============================================================
// Client-side receive  (client build only)
// Fed by the control-channel detour via OnWireReceive.
// ============================================================

#ifdef MODLOADER_CLIENT_BUILD

static void DispatchClientNarrow(const char* narrowData, size_t nbytes)
{
    ParsedEnvelope env = ParseEnvelope(narrowData, nbytes);
    if (!env.valid)
    {
        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] ReceiveFromServer: invalid envelope reason='%S' input=%d bytes",
            env.failureReason ? env.failureReason : "unknown",
            static_cast<int>(nbytes));
        return;
    }

    ModLoaderLogger::LogTrace(
        L"[NetworkChannel] ReceiveFromServer: parsed plugin='%S' tag='%S' declared=%zu decoded=%zu base64=%zu",
        env.pluginName.c_str(),
        env.typeTag.c_str(),
        env.declaredSize,
        env.payload.size(),
        env.encodedPayloadChars);

    // Loader-internal traffic, consumed before any plugin lookup so the reserved
    // name can neither reach a plugin nor be impersonated by one.
    if (env.pluginName == kMlPlugin)
    {
        if (env.typeTag == kMlAck)
            OnManifestAck(env.payload.data(), env.payload.size());
        else if (env.typeTag == kMlEchoBack)
            OnEchoReply(env.payload.data(), env.payload.size());
        else
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Unknown control message '%S' from server", env.typeTag.c_str());
        return;
    }

    // Dispatch to registered handlers
    std::string key = MakeHandlerKey(env.pluginName.c_str(), env.typeTag.c_str());

    std::vector<PluginNetworkMessageCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_handlers.find(key);
        if (it == g_handlers.end())
        {
            ModLoaderLogger::LogTrace(
                L"[NetworkChannel] ReceiveFromServer: no handler for plugin='%S' tag='%S'",
                env.pluginName.c_str(),
                env.typeTag.c_str());
            return;
        }
        callbacks = it->second; // copy snapshot
    }

    ModLoaderLogger::LogTrace(
        L"[NetworkChannel] ReceiveFromServer: dispatching to %zu handler(s) for plugin='%S' tag='%S'",
        callbacks.size(),
        env.pluginName.c_str(),
        env.typeTag.c_str());

    for (auto* cb : callbacks)
    {
        if (!cb) continue;
        try
        {
            cb(env.pluginName.c_str(), env.typeTag.c_str(),
               env.payload.data(), env.payload.size());
        }
        catch (const std::exception& ex)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Exception in handler for plugin='%S' tag='%S': %S",
                env.pluginName.c_str(), env.typeTag.c_str(), ex.what());
        }
        catch (...)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Unknown exception in handler for plugin='%S' tag='%S'",
                env.pluginName.c_str(), env.typeTag.c_str());
        }
    }
}
#endif // MODLOADER_CLIENT_BUILD

// ============================================================
// Authority-side receive  (both builds)
// Fed by the control-channel detour via OnWireReceive, which routes here only
// when this process is the authority (dedicated server, or a client build
// hosting a listen server).
// Returns true if the bytes were a valid mod envelope.
// ============================================================

// senderConn is the authoritative identity here. senderUObject (the sending
// player's controller) is passed on to plugin handlers, but it can still be null
// when a manifest arrives -- a client reports its plugins as soon as its channel
// is up, which can precede the controller being assigned to the connection.
static bool DispatchServerNarrow(void* senderConn, void* senderUObject,
                                 const char* narrowData, size_t nbytes)
{
    ParsedEnvelope env = ParseEnvelope(narrowData, nbytes);
    if (!env.valid)
    {
        ModLoaderLogger::LogWarn(
            L"[NetworkChannel] ReceiveFromClient: invalid envelope from sender=%p reason='%S' input=%d bytes",
            senderUObject,
            env.failureReason ? env.failureReason : "unknown",
            static_cast<int>(nbytes));
        return false;
    }

    ModLoaderLogger::LogTrace(
        L"[NetworkChannel] ReceiveFromClient: parsed sender=%p plugin='%S' tag='%S' declared=%zu decoded=%zu base64=%zu",
        senderUObject,
        env.pluginName.c_str(),
        env.typeTag.c_str(),
        env.declaredSize,
        env.payload.size(),
        env.encodedPayloadChars);

    // Loader-internal traffic, consumed before any plugin lookup so the reserved
    // name can neither reach a plugin nor be impersonated by one.
    if (env.pluginName == kMlPlugin)
    {
        if (env.typeTag == kMlManifest && senderConn)
        {
            auto plugins = DecodeManifest(env.payload.data(), env.payload.size());
            const size_t count = plugins.size();

            for (const auto& rp : plugins)
                ModLoaderLogger::LogDebug(
                    L"[NetworkChannel]   client %p has '%S' version '%S'",
                    senderConn, rp.name.c_str(), rp.version.c_str());

            {
                std::lock_guard<std::mutex> lk(g_manifestMutex);
                g_manifests[senderConn] = std::move(plugins);
            }

            // The other half of the pair above: one INFO line saying what this
            // player turned out to be. Everything finer-grained -- which plugins,
            // which versions -- is Debug, and `clients` shows it on demand.
            std::string senderName = PlayerNameForConnection(senderConn);
            ModLoaderLogger::LogInfo(
                L"[NetworkChannel] Player '%S' is running the mod loader with %zu plugin(s) "
                L"-- acknowledging, plugin data can now flow to them",
                senderName.empty() ? "(unnamed)" : senderName.c_str(), count);

            // It answered, so stop greeting it, and let the ready pump look at
            // what it named.
            {
                std::lock_guard<std::mutex> lk(g_helloMutex);
                g_helloByConn.erase(senderConn);
            }
            MarkReadyDirty();

            // Answer so the client knows it landed. Without this it cannot tell a
            // delivered manifest from one that vanished, and would either retry
            // forever or assume success wrongly.
            std::string ack = MODLOADER_BUILD_TAG;
            ack += kUnitSep;
            ack += std::to_string(PLUGIN_INTERFACE_VERSION);
            ack += kRecordSep;

            if (!SendWire(senderConn, kMlPlugin, BuildEnvelope(kMlPlugin, kMlAck,
                                                    reinterpret_cast<const uint8_t*>(ack.data()),
                                                    ack.size())))
            {
                ModLoaderLogger::LogWarn(
                    L"[NetworkChannel] Could not acknowledge client %p -- it will retry", senderConn);
            }
        }
        else if (env.typeTag == kMlEcho && senderConn)
        {
            // Loopback for the fragmentation self-test. Bounced back verbatim so
            // the client can verify every byte survived a round trip -- which
            // exercises chunking on the way out, reassembly here, chunking again
            // on the reply, and reassembly at the client.
            ModLoaderLogger::LogInfo(
                L"[NetworkChannel] Echo request from %p: %zu bytes -- bouncing back",
                senderConn, env.payload.size());

            if (!SendWire(senderConn, kMlPlugin,
                          BuildEnvelope(kMlPlugin, kMlEchoBack,
                                        env.payload.data(), env.payload.size())))
            {
                ModLoaderLogger::LogWarn(
                    L"[NetworkChannel] Echo reply to %p failed", senderConn);
            }
        }
        else
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Unknown control message '%S' from conn=%p",
                env.typeTag.c_str(), senderConn);
        }
        return true;
    }

    // Dispatch to registered server-side handlers
    std::string key = MakeHandlerKey(env.pluginName.c_str(), env.typeTag.c_str());

    std::vector<PluginNetworkServerMessageCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(g_serverMutex);
        auto it = g_serverHandlers.find(key);
        if (it == g_serverHandlers.end())
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] ReceiveFromClient: no server handler for plugin='%S' tag='%S' sender=%p",
                env.pluginName.c_str(),
                env.typeTag.c_str(),
                senderUObject);
            return true; // valid envelope but no handler -- still consumed
        }
        callbacks = it->second; // copy snapshot
    }

    ModLoaderLogger::LogTrace(
        L"[NetworkChannel] ReceiveFromClient: dispatching to %zu server handler(s) for plugin='%S' tag='%S'",
        callbacks.size(),
        env.pluginName.c_str(),
        env.typeTag.c_str());

    for (auto* cb : callbacks)
    {
        if (!cb) continue;
        try
        {
            cb(senderUObject,
               env.pluginName.c_str(), env.typeTag.c_str(),
               env.payload.data(), env.payload.size());
        }
        catch (const std::exception& ex)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Exception in server handler for plugin='%S' tag='%S': %S",
                env.pluginName.c_str(), env.typeTag.c_str(), ex.what());
        }
        catch (...)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Unknown exception in server handler for plugin='%S' tag='%S'",
                env.pluginName.c_str(), env.typeTag.c_str());
        }
    }

    return true; // mod envelope consumed -- caller should suppress original chat
}

// ============================================================
// Public API
// ============================================================

// ============================================================
// Wire receive
// Called from the UControlChannel::ReceivedBunch detour, on the game thread,
// with the reserved type byte already stripped.
// ============================================================

// ControlChannel caches UNetConnection* -> UControlChannel*, and UE recycles both
// allocations. A stale entry surviving into a new connection at the same address
// would aim a send at a freed channel, so both teardown paths must clear it:
// a departing player on the authority, and a world tearing down (travel,
// disconnect, session end) on either side.
static void OnPlayerLeftForgetConn(void* exitingController)
{
    // Unconditional, and it must stay that way: this is the only cleanup keyed by
    // the controller, and the no-connection branch below is exactly when a pending
    // greeting is left holding a controller the GC is about to take. Doing it
    // first also means the connection-keyed erase below is a no-op rather than a
    // second pass over the same entry.
    ForgetHelloByController(exitingController);

    void* conn = ConnectionForPlayer(exitingController);
    if (conn)
    {
        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Player left -- forgetting peer %p (channel, manifest, "
            L"any half-received message)", conn);
        ForgetConnectionState(conn);
        return;
    }

    // Logout fired but the controller no longer names a connection -- the net
    // layer had already torn it down. The pending greeting is gone (above); there
    // is nothing left to key the rest off, so the periodic sweep and the
    // live-connection check in PumpGreetings are what reclaim this peer's buffers.
    ModLoaderLogger::LogDebug(
        L"[NetworkChannel] Player left with no NetConnection on controller %p; "
        L"greeting dropped, leaving buffer cleanup to the stale-partial sweep",
        exitingController);
}

static void OnWorldEndPlayForgetConns(SDK::UWorld* /*world*/, const char* /*worldName*/)
{
    Hooks::ControlChannel::ForgetAllConnections();
    {
        std::lock_guard<std::mutex> lk(g_reasmMutex);
        g_reassembler.Clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_manifestMutex);
        g_manifests.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_helloMutex);
        g_helloByConn.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_readyMutex);
        g_announced.clear();
        g_notReadyWarned.clear();
        g_readyDirty = !g_clientReadyRegs.empty(); // re-announce into the next world
    }
#ifdef MODLOADER_CLIENT_BUILD
    // Our own report is void too: the next world means a new connection, so the
    // previous acknowledgement no longer says anything about the next one.
    g_manifestSentTo  = nullptr;
    g_manifestSentGen = 0;
    g_manifestAcked   = false;
    g_attempts        = 0;
    g_gaveUpLogged    = false;
    g_lastAttemptMs   = 0;

    // And the next server is a different question entirely. Staying "greeted"
    // across worlds is precisely how a client that once played on a modded
    // server would go on to disconnect itself from a vanilla one.
    g_serverSpeaksWire = false;
    g_serverGreetTag.clear();
    g_worldStartedMs   = 0;
    g_noGreetingLogged = false;
    {
        std::lock_guard<std::mutex> lk(g_srvReadyMutex);
        g_serverReadyFired.clear();
        g_earlySendWarned.clear();
    }
#endif
}

#ifdef MODLOADER_CLIENT_BUILD
// A client reports as soon as it has a world, which is the earliest point its
// connection and control channel are both reliably up.
static void OnWorldBeginPlayReport(SDK::UWorld* /*world*/, const char* /*worldName*/)
{
    // Starts the clock on the not-greeted notice. Set even when we have already
    // been greeted; the notice checks that first and costs nothing then.
    g_worldStartedMs   = GetTickCount64();
    g_noGreetingLogged = false;

    SendManifestToServer(/*force*/ false);
}
#endif

// Route one fully-formed envelope. Reached either directly (small message, one
// bunch) or after reassembly.
static void RouteEnvelope(void* netConnection, const char* narrow, size_t len)
{
    // Routing by authority rather than by direction, because a bunch does not
    // say which way it travelled. While the net mode is still Unknown (no
    // GameState yet, very early in a join) this resolves to the client path.
    if (HasNetAuthority())
    {
        DispatchServerNarrow(netConnection, PlayerForConnection(netConnection), narrow, len);
        return;
    }

#ifdef MODLOADER_CLIENT_BUILD
    DispatchClientNarrow(narrow, len);
#else
    (void)netConnection; (void)narrow; (void)len;
#endif
}

static void OnWireReceive(void* netConnection, const uint8_t* data, size_t len)
{
    if (!data || len == 0) return;

    // A chunk frame and an envelope are trivially distinguishable -- 'MFRG' bytes
    // versus the "[MOD:" prefix -- so no framing flag is needed to tell them apart.
    if (!Fragmentation::IsFragmentFrame(data, len))
    {
        RouteEnvelope(netConnection, reinterpret_cast<const char*>(data), len);
        return;
    }

    const uintptr_t connKey = reinterpret_cast<uintptr_t>(netConnection);
    const uint64_t  nowMs   = GetTickCount64();

    Fragmentation::Completed done;
    Fragmentation::FeedResult result;
    size_t expired = 0;

    {
        std::lock_guard<std::mutex> lk(g_reasmMutex);
        result  = g_reassembler.Feed(connKey, data, len, nowMs, done);
        // Cheap because there are normally no partials at all, and it means a
        // sender that dies mid-message cannot leak a buffer until shutdown.
        expired = g_reassembler.ExpireStale(nowMs);
    }

    if (expired)
        ModLoaderLogger::LogWarn(
            L"[NetworkChannel] Dropped %zu stale partial message(s) -- a sender "
            L"stopped mid-transfer", expired);

    switch (result)
    {
    case Fragmentation::FeedResult::NeedMore:
        ModLoaderLogger::LogTrace(
            L"[NetworkChannel] Fragment accepted from peer %p (%zu bytes), awaiting more",
            netConnection, len);
        return;

    case Fragmentation::FeedResult::Completed:
        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Reassembled %zu bytes from peer %p (label '%S')",
            done.payload.size(), netConnection, done.originalTag.c_str());
        RouteEnvelope(netConnection,
                      reinterpret_cast<const char*>(done.payload.data()),
                      done.payload.size());
        return;

    case Fragmentation::FeedResult::Malformed:
        ModLoaderLogger::LogWarn(
            L"[NetworkChannel] Malformed fragment from peer %p (%zu bytes) -- dropped",
            netConnection, len);
        return;

    case Fragmentation::FeedResult::LimitExceeded:
        ModLoaderLogger::LogWarn(
            L"[NetworkChannel] Fragment from peer %p exceeds configured size/chunk caps -- dropped",
            netConnection);
        return;

    case Fragmentation::FeedResult::NotAFragment:
    default:
        // IsFragmentFrame said otherwise; treat as a plain envelope rather than
        // silently dropping something that might be one.
        RouteEnvelope(netConnection, reinterpret_cast<const char*>(data), len);
        return;
    }
}

// Per-frame housekeeping. Registered on both builds: the stale-partial sweep is
// an authority-side concern, and the manifest refresh is a client-side one.
static void OnEngineTick(float /*deltaSeconds*/)
{
    const uint64_t now = GetTickCount64();

    SweepStalePartials(now);

    // Authority side, and both are no-ops with nothing outstanding: greetings
    // waiting on an answer, and (client, plugin) pairs waiting to be announced.
    PumpGreetings(now);
    PumpClientReady();

#ifdef MODLOADER_CLIENT_BUILD
    RefreshManifestIfStale();
#endif
}

// The authority greets a client here rather than anywhere later: it is the first
// point at which the controller exists and is owned by the connection. It is not
// necessarily a point at which the RPC will arrive, which is what the retry in
// PumpGreetings is for.
static void OnPlayerJoinedGreet(void* playerController)
{
    BeginGreeting(playerController);
}

// Best-effort player name for a connection. Resolved at query time rather than
// stored: a manifest can arrive before the controller (and therefore the name)
// exists, so a value captured at receipt would often be empty forever.
// The __try block must not construct or destroy a C++ object (MSVC C2712), so it
// only fills a POD buffer -- every std::string lives outside it.
static void CopyPlayerNameSEH(void* conn, wchar_t* buf, size_t bufChars)
{
    __try
    {
        auto* nc = reinterpret_cast<SDK::UNetConnection*>(conn);
        if (!nc || !nc->PlayerController) return;

        SDK::APlayerState* ps = nc->PlayerController->PlayerState;
        if (!ps) return;

        const wchar_t* src = ps->PlayerNamePrivate.CStr();
        if (!src) return;

        wcsncpy_s(buf, bufChars, src, _TRUNCATE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        buf[0] = L'\0';
    }
}

static std::string PlayerNameForConnection(void* conn)
{
    wchar_t buf[128] = {};
    CopyPlayerNameSEH(conn, buf, 128);
    if (!buf[0]) return std::string();

    char narrow[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return std::string(narrow);
}

std::vector<NetworkChannel::ClientManifest> NetworkChannel::GetClientManifests()
{
    std::vector<ClientManifest> out;
    if (!HasNetAuthority()) return out;

    // Enumerate live controllers so a connected client that has NOT reported
    // still shows up -- "who is missing a manifest" is the question this is most
    // often asked to answer.
    SDK::UWorld* world = SDK::UWorld::GetWorld();
    if (!world) return out;

    SDK::TArray<SDK::AActor*> actors;
    SDK::UGameplayStatics::GetAllActorsOfClass(
        world, SDK::ACrPlayerControllerBase::StaticClass(), &actors);

    // Snapshot the greeting state BEFORE taking the manifest lock. Reading it
    // inside the loop would hold manifest->hello, and PumpGreetings is careful
    // never to hold those two together for exactly that reason.
    std::unordered_map<void*, int> attempts;
    {
        std::lock_guard<std::mutex> lk(g_helloMutex);
        for (const auto& kv : g_helloByConn)
            attempts[kv.first] = kv.second.attempts;
    }

    std::lock_guard<std::mutex> lk(g_manifestMutex);

    for (int32_t i = 0; i < actors.Num(); ++i)
    {
        void* conn = ConnectionForPlayer(actors[i]);
        if (!conn) continue;   // listen host's own controller

        ClientManifest entry;
        entry.connection       = conn;
        entry.playerName       = PlayerNameForConnection(conn);
        entry.greetingAttempts = attempts.count(conn) ? attempts[conn] : 0;

        auto it = g_manifests.find(conn);
        if (it != g_manifests.end())
        {
            entry.reported = true;
            entry.plugins  = it->second;
        }
        out.push_back(std::move(entry));
    }

    return out;
}

bool NetworkChannel::StartFragmentationTest(size_t bytes, std::string& err)
{
#ifdef MODLOADER_CLIENT_BUILD
    return StartEchoTest(bytes, err);
#else
    (void)bytes;
    err = "Only a connected client can run the echo test.";
    return false;
#endif
}

IPluginNetworkChannel* NetworkChannel::GetInterface()
{
    return &g_networkIface;
}

// Two call sites reach Initialize -- the startup phase, and the lazy path in
// GetPluginHooks() that predates it -- and the registrations below are not
// idempotent (each Register* appends). The guard lives here rather than at
// either call site so a third caller cannot reintroduce the problem.
static bool g_networkInitialized = false;

void NetworkChannel::Initialize()
{
    if (g_networkInitialized) return;
    g_networkInitialized = true;

    // The control channel is the whole transport now: this detour is both the
    // only receive path and the prerequisite for every send.
    Hooks::ControlChannel::SetReceiveCallback(&OnWireReceive);
    if (!Hooks::ControlChannel::IsInstalled())
        Hooks::ControlChannel::Install();

    if (Hooks::ControlChannel::IsAvailable())
        ModLoaderLogger::LogInfo(L"[NetworkChannel] Control-channel wire ready");
    else
        ModLoaderLogger::LogError(
            L"[NetworkChannel] Control-channel wire UNAVAILABLE -- plugin networking is dead this "
            L"session and there is no fallback transport. The six control-channel patterns are "
            L"REQUIRED at preflight, so reaching this line means the scan succeeded but the hook "
            L"install did not; see the [ControlChannel] errors above.");

#ifdef MODLOADER_CLIENT_BUILD
    // The other half of the handshake. Installed even on a client that will go on
    // to host: a listen host is still greeted by nobody, and the hook is inert
    // when no greeting arrives.
    Hooks::ModLoaderHello::SetCallback(&OnServerGreeting);
    if (!Hooks::ModLoaderHello::Install())
        ModLoaderLogger::LogError(
            L"[NetworkChannel] Could not listen for the authority's greeting -- this client will "
            L"never report its plugins, so networked plugins will do nothing. It will NOT be "
            L"disconnected, which is the failure mode this is designed to have.");
#endif

    // These lazily install their own hooks on first registration.
    Hooks::PlayerLeft::RegisterPluginCallback(&OnPlayerLeftForgetConn);
    Hooks::PlayerJoined::RegisterPluginCallback(&OnPlayerJoinedGreet);
    Hooks::WorldEndPlay::RegisterBeforeCallback(&OnWorldEndPlayForgetConns);
    Hooks::EngineTick::RegisterPluginCallback(&OnEngineTick);
#ifdef MODLOADER_CLIENT_BUILD
    Hooks::WorldBeginPlay::RegisterAnyWorldCallback(&OnWorldBeginPlayReport);
#endif
}

void NetworkChannel::Shutdown()
{
    g_networkInitialized = false;

    Hooks::PlayerLeft::UnregisterPluginCallback(&OnPlayerLeftForgetConn);
    Hooks::PlayerJoined::UnregisterPluginCallback(&OnPlayerJoinedGreet);
    Hooks::WorldEndPlay::UnregisterBeforeCallback(&OnWorldEndPlayForgetConns);
    Hooks::EngineTick::UnregisterPluginCallback(&OnEngineTick);
#ifdef MODLOADER_CLIENT_BUILD
    Hooks::WorldBeginPlay::UnregisterAnyWorldCallback(&OnWorldBeginPlayReport);
    Hooks::ModLoaderHello::Remove();
#endif

    Hooks::ControlChannel::Remove();

    {
        std::lock_guard<std::mutex> lk(g_reasmMutex);
        g_reassembler.Clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_manifestMutex);
        g_manifests.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_helloMutex);
        g_helloByConn.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_readyMutex);
        g_clientReadyRegs.clear();
        g_announced.clear();
        g_notReadyWarned.clear();
        g_readyDirty = false;
    }

    {
        std::lock_guard<std::mutex> lk(g_serverMutex);
        g_serverHandlers.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_excludeMutex);
        g_excludedControllers.clear();
    }

#ifdef MODLOADER_CLIENT_BUILD
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_handlers.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_srvReadyMutex);
        g_serverReadyCbs.clear();
        g_serverReadyFired.clear();
        g_earlySendWarned.clear();
    }
    g_serverSpeaksWire = false;
    g_serverGreetTag.clear();
    ModLoaderLogger::LogInfo(L"[NetworkChannel] Client network channel shut down");
#else
    ModLoaderLogger::LogInfo(L"[NetworkChannel] Server network channel shut down");
#endif
}

#else // Generic (plain Debug/Release) build

IPluginNetworkChannel* NetworkChannel::GetInterface() { return nullptr; }
void NetworkChannel::Initialize() {}
void NetworkChannel::Shutdown() {}
std::vector<NetworkChannel::ClientManifest> NetworkChannel::GetClientManifests() { return {}; }
bool NetworkChannel::StartFragmentationTest(size_t, std::string& err)
{
    err = "Plugin networking is not present in this build.";
    return false;
}

#endif // MODLOADER_SERVER_BUILD || MODLOADER_CLIENT_BUILD
