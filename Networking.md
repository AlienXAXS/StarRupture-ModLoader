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

- Maximum reassembled size per logical message is capped by the receiver (8 MB);
  a larger send is dropped there. There is no accessor for it -- an earlier draft of
  this document named a `GetMaxReassembledBytes()` that was never added.
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

## When can I send? (readiness -- interface v56)

**A player-joined hook is too early.** A packet sent from one is dropped: the
client has not yet told the server which plugins it has, and until it does the
server will not route anything to it. This is the single most common way to lose a
packet, and since v56 it warns once per plugin per client in `modloader.log`.

Send join-time state from the ready callback instead:

```cpp
// server: fires once per client that has YOUR plugin at YOUR version
hooks->Network->RegisterClientReadyCallback(self, [](void* playerController) {
    Packet::SendToClient(hooks, self, playerController, WorldStateSnapshot{ ... });
});

// client: fires once the server has acknowledged us
hooks->Network->RegisterServerReadyCallback(self, [](const char* serverBuildTag) {
    Packet::SendToServer(hooks, self, HelloFromPlugin{ ... });
});
```

Both have a polling equivalent -- `IsClientReady(pc, self)` and `IsServerReady()`
-- and both callbacks fire immediately for anything already ready when you
register, so a plugin loaded or reloaded mid-session does not miss the clients
already in it. Unregister in `PluginShutdown`.

The loader does **not** queue packets sent before a peer is ready and replay them
later. It cannot judge whether a payload is still meaningful once the moment has
passed -- a position update replayed three seconds late is worse than one that was
dropped -- so that decision stays with you. Send from the callback, or send on
request; do not send hopefully.

Readiness is per plugin, not per client: it means *this client reported your
plugin at your exact version*. A client running the loader but not your plugin, or
a different build of it, never becomes ready to you.

## Transport backing (implementation note)

One transport: payloads ride directly on the Unreal per-connection control channel
(`UControlChannel`), independent of any game UFUNCTION. The older transport that
smuggled envelopes through replicated game RPCs was deleted in v54, not kept as a
fallback. See `network_channel/CONTROL_CHANNEL_WIRE.md`.

Nothing is sent to a peer before it has identified itself as running this loader,
and that is not a preference -- an unrecognised control message makes the engine
**close the connection**, so a speculative send to a vanilla peer disconnects it.
The handshake that establishes this cannot itself use the control channel, so it
does not: the server greets each joining client through an ordinary replicated
engine RPC (`APlayerController::ClientMessage`, inert on a client with no loader),
and only a client that has been greeted ever puts anything on the wire. A client
that joins a server without the loader is never greeted, stays silent, and plays
on normally with its networked plugins inactive.

This is why readiness above is a real state a plugin has to respect rather than a
formality: for a peer that never completes the handshake, it never arrives.
