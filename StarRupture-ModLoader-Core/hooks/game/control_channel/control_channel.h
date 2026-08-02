#pragma once

#include "../../hooks_common.h"
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Control-channel wire  (all build targets)
//
// Sends and receives raw modloader payloads directly on the Unreal Engine
// per-connection control channel (UControlChannel), instead of piggybacking on a
// game UFUNCTION. See network_channel/CONTROL_CHANNEL_WIRE.md for the reverse-
// engineering that backs the native calls and offsets used here.
//
// A modloader message is one control bunch: a reserved leading byte
// (kWireMsgType = 0xC0, above every message type the engine's ReceivedBunch
// switch handles, 0x00..0x21) followed by the payload bytes. The receive detour
// consumes bunches that start with our byte and passes every other bunch through
// to the original UControlChannel::ReceivedBunch untouched, so vanilla engine
// traffic is unaffected.
//
// This layer is byte-oriented and knows nothing about plugins or fragmentation --
// network_channel.cpp drives it. If any required native fails to resolve,
// IsAvailable() returns false and nothing is sent -- there is no second
// transport. In practice preflight rejects the missing pattern and the loader
// disables itself before this point, so IsAvailable() being false is a
// last-resort guard rather than a mode anything is expected to run in.
// ---------------------------------------------------------------------------

namespace Hooks::ControlChannel
{
    // Reserved control-message type for modloader traffic.
    inline constexpr uint8_t kWireMsgType = 0xC0;

    // Resolve the native functions and install the ReceivedBunch detour.
    // Returns true if the hook installed (send may still be unavailable if a
    // send-side native failed to resolve -- check IsAvailable()).
    bool Install();
    void Remove();
    bool IsInstalled();

    // True only if every native needed for send AND receive resolved.
    bool IsAvailable();

    // Send one payload as a control bunch to the given UControlChannel*.
    // Returns false if the wire is unavailable or controlChannel is null.
    bool SendRaw(void* controlChannel, const uint8_t* data, size_t len);

    // Find the UControlChannel* for a UNetConnection* (fast path: a map captured
    // by the receive detour; fallback: scan the connection's OpenChannels).
    // Returns nullptr if none is open yet.
    void* GetControlChannel(void* netConnection);

    // Called from the detour when a modloader bunch arrives. netConnection is the
    // sender's UNetConnection* (UControlChannel::Connection); the type byte has
    // already been stripped. Invoked on the game thread.
    using RawReceiveCallback = void(*)(void* netConnection, const uint8_t* data, size_t len);
    void SetReceiveCallback(RawReceiveCallback cb);

    // Drop the captured UControlChannel mapping for a connection that is closing.
    void ForgetConnection(void* netConnection);

    // Drop every captured mapping. For world teardown, where the connections are
    // going away wholesale and enumerating them individually is not possible.
    void ForgetAllConnections();
}
