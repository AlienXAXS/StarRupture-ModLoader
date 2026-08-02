# Plugin Networking

The modloader exposes a bidirectional message channel between the server and each
connected client, available to plugins through `hooks->Network`
(`IPluginNetworkChannel`). This document covers what a plugin author needs; the
internals are in `StarRupture-ModLoader-Core\network_channel\`.

`hooks->Network` is non-null on **client** and **server** builds and `nullptr` on
generic builds -- always null-check.

## Two ways to send

### 1. Fixed-size POD packets -- `plugin_network_helpers.h`

For a message that is a plain fixed-layout struct (no pointers, no `std::` members):

```cpp
struct TimerPacket { float time; int32_t alive; uint8_t phase; uint8_t pad[3]; };

// server:
Network::SendPacketToAllClients(hooks, self, TimerPacket{ ... });
// client:
Network::OnReceive<TimerPacket>(hooks, self, [](const TimerPacket& p){ ... });
```

The struct is `memcpy`'d, so it must be trivially copyable. Add explicit padding
for a deterministic layout across builds.

### 2. Variable-length packets received as objects -- `plugin_packet.h`

When a message contains strings, blobs, or arrays, define it as a packet with
`Write`/`Read` methods and a stable id:

```cpp
struct ChatBroadcast {
    static constexpr uint32_t kPacketId = 0x00C0FFEE; // your stable id
    static constexpr uint16_t kVersion  = 1;
    int32_t              senderId;
    std::string          message;
    std::vector<uint8_t> icon;

    void Write(Packet::Writer& w) const { w.I32(senderId); w.Str(message); w.Blob(icon); }
    bool Read(Packet::Reader& r) { r.I32(senderId); r.Str(message); r.Blob(icon); return r.Ok(); }
};

// server:
Packet::SendToAllClients(hooks, self, ChatBroadcast{ ... });
Packet::OnServerReceive<ChatBroadcast>(hooks, self,
    [](void* senderPC, const ChatBroadcast& p){ ... });

// client:
Packet::SendToServer(hooks, self, ChatBroadcast{ ... });
Packet::OnReceive<ChatBroadcast>(hooks, self,
    [](const ChatBroadcast& p){ ... });
```

`Reader` is fully bounds-checked: a truncated or hostile buffer sets `Ok() == false`
and never over-reads. A frame whose `kPacketId`/`kVersion` does not match the
receiving type is dropped before your callback runs. Never renumber a shipped
`kPacketId` or reorder `Write`/`Read` fields without bumping `kVersion`.

Both `OnReceive<T>` and `OnServerReceive<T>` allow **one active handler per type per
plugin** (last registration wins). They return the raw callback pointer so you can
`Unregister...` it in `PluginShutdown`.

## Large payloads (fragmentation)

As of interface **v50**, the loader transparently splits any payload larger than a
single transport chunk into multiple chunk frames and reassembles them on the far
side. Your send call takes the whole payload and your handler receives the whole
payload -- you never see a fragment. This works for both APIs above and for the raw
`SendPacket*` calls.

- Maximum reassembled size per logical message: `hooks->Network->GetMaxReassembledBytes()`
  (default 8 MB). A send larger than the receiver's cap is dropped by the receiver.
- Delivery is all-or-nothing: the handler fires once, only after every chunk has
  arrived. Incomplete messages are dropped after a timeout, and all partials for a
  player are dropped when that player disconnects.
- Ordering/reliability are provided by the underlying transport; you do not need to
  ack or retransmit.

Large or high-frequency payloads still consume bandwidth on the shared channel --
keep them reasonable.

## Reserved names

The tag `"$MODFRAG"` is reserved for fragment frames -- do not register a raw message
handler under that tag. `plugin_packet.h` uses tags of the form `"PKT:<hexId>"`.

## Transport backing (implementation note)

There are two transports underneath, chosen automatically per peer -- none of this
changes the plugin-facing API above:

1. **Control-channel wire** (preferred): payloads ride directly on the Unreal
   per-connection control channel (`UControlChannel`), independent of any game
   UFUNCTION. See `network_channel/CONTROL_CHANNEL_WIRE.md`.
2. **Legacy UFUNCTION envelope** (fallback): an FString round-tripped through a
   replicated game function, base64 inside a `[MOD:...]` envelope.

The wire is only used toward a peer once that peer has been confirmed to also run
this modloader, via a capability handshake exchanged over the safe legacy
transport (a modded client announces itself; the server replies). Until confirmed
-- and whenever the far end is vanilla or an older loader -- traffic uses the
legacy transport, which is safe across versions. This gating is deliberate:
sending a modloader control bunch to a peer that isn't intercepting it would make
the engine drop the connection, so we never do it speculatively.

Both ends must run this modloader for the wire to engage; otherwise everything
still works over the legacy path.
