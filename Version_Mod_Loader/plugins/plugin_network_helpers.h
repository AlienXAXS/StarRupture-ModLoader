#pragma once

// ============================================================
// plugin_network_helpers.h  (v18)
//
// Typed packet helpers for the IPluginNetworkChannel API.
//
// Usage (plugin author):
//
//   // 1. Define a POD struct for your packet.  No pointers, no virtual
//   //    functions, no std containers.  Add explicit padding for a
//   //    deterministic, cross-platform layout.
//   struct TimerPacket {
//       float   currentTime;
//       int32_t playersAlive;
//       uint8_t phase;
//       uint8_t pad[3];
//   };
//
//   // 2. In PluginInit, check hooks->Network and register by build side:
//   if (self->hooks->Network) {
//       if (self->hooks->Network->IsServer()) {
//           self->hooks->Engine->RegisterOnTick([](float) {
//               TimerPacket pkt{ GetTime(), CountPlayers(), GetPhase(), {} };
//               Network::SendPacketToAllClients(g_self->hooks, g_self, pkt);
//           });
//       } else {
//           Network::OnReceive<TimerPacket>(self->hooks, self,
//               [](const TimerPacket& p) {
//                   g_display.time  = p.currentTime;
//                   g_display.alive = p.playersAlive;
//               });
//       }
//   }
//
// OnReceive<T> limitations:
//   One active handler per packet type T per plugin.  Calling OnReceive<T>
//   again replaces the previous callback.  For multiple handlers on the same
//   type, call hooks->Network->RegisterMessageHandler() directly.
//
// Payload size:
//   Keep payloads under ~1 KB.  The loader logs a warning above 1400 bytes.
// ============================================================

#include "plugin_interface.h"
#include <functional>
#include <cstring>
#include <type_traits>
#include <typeinfo>

namespace Network
{

// ----------------------------------------------------------------
// SendPacketToPlayer<T>
// Server-side: send a typed packet to a single player.
//   hooks            : the IPluginHooks* from PluginInit (or g_self->hooks)
//   self             : the calling plugin's IPluginSelf* (name used for routing)
//   playerController : the APlayerController* for the target player (void*)
//   pkt              : the packet to send
// ----------------------------------------------------------------
template<typename T>
void SendPacketToPlayer(IPluginHooks* hooks, const IPluginSelf* self,
                        void* playerController, const T& pkt)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::SendPacketToPlayer<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network) return;
    hooks->Network->SendPacketToClient(
        playerController,
        self,
        typeid(T).name(),
        reinterpret_cast<const uint8_t*>(&pkt),
        sizeof(T));
}

// ----------------------------------------------------------------
// SendPacketToAllClients<T>
// Server-side: broadcast a typed packet to all connected players.
//   hooks : the IPluginHooks* from PluginInit (or g_self->hooks)
//   self  : the calling plugin's IPluginSelf* (name used for routing)
//   pkt   : the packet to broadcast
// ----------------------------------------------------------------
template<typename T>
void SendPacketToAllClients(IPluginHooks* hooks, const IPluginSelf* self, const T& pkt)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::SendPacketToAllClients<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network) return;
    hooks->Network->SendPacketToAllClients(
        self,
        typeid(T).name(),
        reinterpret_cast<const uint8_t*>(&pkt),
        sizeof(T));
}

// ----------------------------------------------------------------
// OnReceive<T>
// Client-side: register a typed handler for incoming packets.
//   hooks : the IPluginHooks* from PluginInit (or g_self->hooks)
//   self  : the calling plugin's IPluginSelf* -- name must match the sender's
//   cb    : callback invoked with a const T& on each matching packet.
//           Called from the game thread.
//
// Returns the raw PluginNetworkMessageCallback pointer so it can be
// passed to hooks->Network->UnregisterMessageHandler during PluginShutdown.
// ----------------------------------------------------------------
template<typename T>
PluginNetworkMessageCallback OnReceive(IPluginHooks* hooks, const IPluginSelf* self,
                                       std::function<void(const T&)> cb)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::OnReceive<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network || !cb) return nullptr;

    // Per-T static storage -- each template instantiation gets its own slot.
    // Calling OnReceive<T> again replaces the stored callback (last-write wins).
    struct Handler
    {
        static std::function<void(const T&)>& Callback()
        {
            static std::function<void(const T&)> s;
            return s;
        }

        static void Dispatch(const char* /*pluginName*/, const char* /*typeTag*/,
                             const uint8_t* data, size_t size)
        {
            if (size != sizeof(T)) return; // size guard against malformed packets
            T pkt;
            std::memcpy(&pkt, data, sizeof(T));
            auto& fn = Callback();
            if (fn) fn(pkt);
        }
    };

    Handler::Callback() = std::move(cb);
    hooks->Network->RegisterMessageHandler(self, typeid(T).name(), &Handler::Dispatch);
    return &Handler::Dispatch;
}

// ----------------------------------------------------------------
// SendPacketToServer<T>
// Client-side: send a typed packet to the server.
//   hooks : the IPluginHooks* from PluginInit (or g_self->hooks)
//   self  : the calling plugin's IPluginSelf* (name used for routing)
//   pkt   : the packet to send
// ----------------------------------------------------------------
template<typename T>
void SendPacketToServer(IPluginHooks* hooks, const IPluginSelf* self, const T& pkt)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::SendPacketToServer<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network) return;
    hooks->Network->SendPacketToServer(
        self,
        typeid(T).name(),
        reinterpret_cast<const uint8_t*>(&pkt),
        sizeof(T));
}

// ----------------------------------------------------------------
// OnServerReceive<T>
// Server-side: register a typed handler for Client->Server packets.
//   hooks : the IPluginHooks* from PluginInit (or g_self->hooks)
//   self  : the calling plugin's IPluginSelf* -- name must match the sender's
//   cb    : callback invoked with senderPlayerController (void*) and
//           a const T& on each matching packet.
//           Called from the game thread (ServerChatCommit detour).
//
// Returns the raw PluginNetworkServerMessageCallback pointer so it can be
// passed to hooks->Network->UnregisterServerMessageHandler during PluginShutdown.
// ----------------------------------------------------------------
template<typename T>
PluginNetworkServerMessageCallback OnServerReceive(
    IPluginHooks* hooks,
    const IPluginSelf* self,
    std::function<void(void* senderPC, const T&)> cb)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::OnServerReceive<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network || !cb) return nullptr;

    // Per-T static storage -- each template instantiation gets its own slot.
    struct Handler
    {
        static std::function<void(void*, const T&)>& Callback()
        {
            static std::function<void(void*, const T&)> s;
            return s;
        }

        static void Dispatch(void* senderPC,
                             const char* /*pluginName*/, const char* /*typeTag*/,
                             const uint8_t* data, size_t size)
        {
            if (size != sizeof(T)) return; // size guard against malformed packets
            T pkt;
            std::memcpy(&pkt, data, sizeof(T));
            auto& fn = Callback();
            if (fn) fn(senderPC, pkt);
        }
    };

    Handler::Callback() = std::move(cb);
    hooks->Network->RegisterServerMessageHandler(self, typeid(T).name(), &Handler::Dispatch);
    return &Handler::Dispatch;
}

} // namespace Network
