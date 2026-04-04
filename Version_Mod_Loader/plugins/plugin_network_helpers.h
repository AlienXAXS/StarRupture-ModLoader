#pragma once

// ============================================================
// plugin_network_helpers.h  (v17)
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
//   if (hooks->Network) {
//       if (hooks->Network->IsServer()) {
//           hooks->Engine->RegisterOnTick([](float) {
//               TimerPacket pkt{ GetTime(), CountPlayers(), GetPhase(), {} };
//               Network::SendPacketToAllPlayers(g_hooks, "MyPlugin", pkt);
//           });
//       } else {
//           Network::OnReceive<TimerPacket>(g_hooks, "MyPlugin",
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
//   hooks            : the IPluginHooks* from PluginInit
//   pluginName       : your plugin's name (from GetPluginInfo()->name)
//   playerController : the APlayerController* for the target player (void*)
//   pkt              : the packet to send
// ----------------------------------------------------------------
template<typename T>
void SendPacketToPlayer(IPluginHooks* hooks, const char* pluginName,
                        void* playerController, const T& pkt)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::SendPacketToPlayer<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network) return;
    hooks->Network->SendPacketToClient(
        playerController,
        pluginName,
        typeid(T).name(),
        reinterpret_cast<const uint8_t*>(&pkt),
        sizeof(T));
}

// ----------------------------------------------------------------
// SendPacketToAllPlayers<T>
// Server-side: broadcast a typed packet to all connected players.
//   hooks      : the IPluginHooks* from PluginInit
//   pluginName : your plugin's name (from GetPluginInfo()->name)
//   pkt        : the packet to broadcast
// ----------------------------------------------------------------
template<typename T>
void SendPacketToAllPlayers(IPluginHooks* hooks, const char* pluginName, const T& pkt)
{
    static_assert(std::is_trivially_copyable_v<T>,
        "Network::SendPacketToAllPlayers<T>: T must be trivially copyable "
        "(no pointers, vtables, or std containers)");
    if (!hooks || !hooks->Network) return;
    hooks->Network->SendPacketToAllPlayers(
        pluginName,
        typeid(T).name(),
        reinterpret_cast<const uint8_t*>(&pkt),
        sizeof(T));
}

// ----------------------------------------------------------------
// OnReceive<T>
// Client-side: register a typed handler for incoming packets.
//   hooks      : the IPluginHooks* from PluginInit
//   pluginName : your plugin's name -- must match the sender's pluginName
//   cb         : callback invoked with a const T& on each matching packet.
//                Called from the game thread.
//
// Returns the raw PluginNetworkMessageCallback pointer so it can be
// passed to hooks->Network->UnregisterMessageHandler during PluginShutdown.
// ----------------------------------------------------------------
template<typename T>
PluginNetworkMessageCallback OnReceive(IPluginHooks* hooks, const char* pluginName,
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
    hooks->Network->RegisterMessageHandler(pluginName, typeid(T).name(), &Handler::Dispatch);
    return &Handler::Dispatch;
}

} // namespace Network
