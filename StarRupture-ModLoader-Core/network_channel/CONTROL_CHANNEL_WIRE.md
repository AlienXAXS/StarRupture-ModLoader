# Control-Channel Wire -- reverse-engineering notes

These are the native primitives used to replace the UFUNCTION-envelope transport
with a real Unreal control-channel wire. Addresses below are anchors for locating
the functions in IDA and differ on every patch -- they are NOT hardcodable.

## Status (2026-08-02)

The live AOBs are in `hooks/game/scan_patterns.h`; the consumer is
`hooks/game/control_channel/`; `network_channel.cpp` routes through it with a
legacy fallback gated by the `$CC`/`hi` handshake. Everything below has been
re-verified against the shipping binaries, and several earlier notes were wrong:

- **The six AOBs are identical in the client and server exes.** There is one set
  of patterns, no per-build variants.
- **`FOutBunch::~FOutBunch` is matched directly**, not through a call-site xref.
  It is a real 0x53-byte function with a (long) unique entry signature. The
  `matchAddr + 5 + rel32` indirection described in earlier notes is gone.
- **`UControlChannel::SendBunch` really does take a hidden return pointer.**
  `FPacketIdRange` is 8 bytes, which would normally come back in RAX, but IDA's
  argument analysis and the function's own prologue agree: `this`=RCX,
  `result`=RDX, `Bunch`=R8 (`49 8B F0`), `bMerge`=R9.
- **`FBitReader::SerializeBits` (bits) and `FBitReader::Serialize` (bytes) both
  exist** with near-identical mangled names. The wire uses SerializeBits and
  passes bit counts. `FBitWriter::Serialize` takes BYTES. Do not mix these up.
- Struct offsets `UChannel::Connection`@0x28, `UNetConnection::OpenChannels`
  Data@0x70/Num@0x78, and `UObject::Class`@0x10 are confirmed against the SDK.

**Verified on-target (2026-08-02):** packets flow over the wire, client plugin
manifests are received and gate delivery, and unload/reload correctly triggers a
re-report via the plugin generation counter. The RE below is confirmed working
against the shipping build, not just read out of a disassembler.

## Functions

| Function | Mangled | Dump addr | Signature |
|---|---|---|---|
| `UControlChannel::ReceivedBunch` | `?ReceivedBunch@UControlChannel@@UEAAXAEAVFInBunch@@@Z` | 0x14499E8A0 | `void(UControlChannel* this, FInBunch& Bunch)` -- **hook target** (virtual) |
| `UControlChannel::SendBunch` | `?SendBunch@UControlChannel@@UEAA?AUFPacketIdRange@@PEAVFOutBunch@@_N@Z` | 0x1449ACFC0 | `FPacketIdRange* (UControlChannel* this, FPacketIdRange* result, FOutBunch* Bunch, bool bMerge)` |
| `UChannel::SendBunch` | `?SendBunch@UChannel@@UEAA?AUFPacketIdRange@@PEAVFOutBunch@@_N@Z` | 0x1449ABCF0 | same shape (base; UControlChannel::SendBunch tail-calls it) |
| `FControlChannelOutBunch::ctor` | `??0FControlChannelOutBunch@@QEAA@PEAVUChannel@@_N@Z` | 0x14497AC60 | `void(FControlChannelOutBunch* this, UChannel* InChannel, bool bClose)` |
| `FOutBunch::~FOutBunch` | `??1FOutBunch@@UEAA@XZ` | 0x14497C810 | `void(FOutBunch* this)` -- frees buffers |
| `FBitWriter::Serialize` | `?Serialize@FBitWriter@@UEAAXPEAX_J@Z` | 0x1414965F0 | `void(FBitWriter* this, void* src, int64 numBytes)` -- append bytes |
| `FBitReader::Serialize` | `?Serialize@FBitReader@@UEAAXPEAX_J@Z` | 0x1414965D0 | `void(FBitReader* this, void* dest, int64 numBytes)` -- read bytes |

Note `FControlChannelOutBunch` ctor internally calls `FOutBunch::FOutBunch(UChannel*,bool)`
and sets it reliable, so we never construct the raw FOutBunch ourselves.

Server-exe addresses for the same functions (clean, unpacked -- the easier one to
open): ReceivedBunch `0x144134210`, UControlChannel::SendBunch `0x144142B10`,
FControlChannelOutBunch ctor `0x1441105D0`, ~FOutBunch `0x144112180`,
FBitWriter::Serialize `0x14140BE90`, FBitReader::SerializeBits `0x14140CA60`.

## Struct offsets (FBitWriter == FOutBunch base; FBitReader == FInBunch base)

Cross-verified across ReceivedBunch / SendBunch / Serialize / SerializeBits:

| Offset | Field | Notes |
|---|---|---|
| 0x29 | FArchiveState flags byte | bit 0x2 = `ArIsError` (overflow) |
| 0x90 | `Buffer` TArray<uint8> data ptr | the bit buffer |
| 0x98 | `Buffer` TArray Num (int32) | buffer size in **bytes** |
| 0xA0 | `Num` (int64) | number of **bits** written (writer) / total bits (reader) |
| 0xA8 | writer: `Max` (int64) / reader: `Pos` (int64) | reader bit cursor lives here |
| 0xF4 | FOutBunch flags byte | control bunch ORs 0x10; bit 0x4 used in NumOutRec calc |

`UChannel::Connection` (UNetConnection*) is at `this+0x28` (matches SDK `UChannel`).
Byte count from bits = `(Num + 7) >> 3` (as the engine does in SendBunch).
FOutBunch total size ~0x130+; allocate 0x400 zeroed to be safe.

## Reserved message type

`UControlChannel::ReceivedBunch` reads a leading uint8 `MessageType` then dispatches
through a **34-case switch on values 0x00..0x21**. Anything >= 0x22 is not a real
engine/game control message, so a reserved type of **0xC0** is safe: the engine
never legitimately produces it, and our detour consumes it before the switch. Gate
real use behind a capability handshake so a vanilla peer is never sent one.

**The handshake (2026-08-05).** The gate above was not implemented when the wire
shipped, and the consequence was exactly the predicted one: a client joining a
server without the loader sent its plugin manifest unprompted, and the server
answered with

```
LogNet: UNetConnection::SendCloseReason:
LogNet:  - Result=ControlChannelMessageUnknown, ErrorContext="ControlChannelMessageUnknown"
```

The handshake cannot use this channel -- that is the whole difficulty, and why the
original commit concluded negotiation was impossible. It is impossible *in band*
only. The authority now greets each joining client with
`APlayerController::ClientMessage`, a replicated engine RPC that reaches a peer
running no loader at all without harming it (`ClientTeamMessage_Implementation` at
`0x144f8d1b0` ends at `ViewportConsole->OutputText`, and shipping never creates a
`ViewportConsole` -- see the console-glue findings). Only a greeted client speaks
on the wire. Implementation: `hooks/game/modloader_hello/`.

An alternative worth recording, since it will look attractive again: **NMT_DebugText
(type 17)** would let the greeting ride the control channel itself, needing no new
hook at all. It was not taken because it depends on 17 being registered in this
build -- `FNetControlMessageInfo::IsRegistered` is what decides between "skip
harmlessly" and "close the connection" -- and that was not confirmed. The UE_LOG
format strings for the DebugText path are absent from the binary's string table,
which is not evidence either way (they are wide, and the mangling makes them
unsearchable). If it is wrong, the failure mode is disconnecting vanilla clients:
the same bug, mirrored. The RPC route has no equivalent risk, because a replicated
UFUNCTION has no "unknown message" path.

## Send recipe (per target UControlChannel* cc)

1. `char bunch[0x400] = {0};`
2. `FControlChannelOutBunch_ctor(bunch, cc, /*bClose*/false);`
3. First byte = 0xC0 (our type), then our fragment/frame bytes:
   `FBitWriter_Serialize(bunch, &kType, 1); FBitWriter_Serialize(bunch, data, len);`
4. Check no overflow: `((uint8*)bunch)[0x29] & 0x2` must be 0.
5. `FPacketIdRange r; UControlChannel_SendBunch(cc, &r, (FOutBunch*)bunch, false);`
6. `FOutBunch_dtor(bunch);`  (frees the buffer allocations)

Optionally `UNetConnection::FlushNet` for immediate send (AOB not yet pulled).

## Receive recipe (detour of UControlChannel::ReceivedBunch)

```
savedPos = *(int64*)(bunch + 0xA8);
num      = *(int64*)(bunch + 0xA0);
if (num - savedPos >= 8 && !(bunch[0x29] & 0x2)) {
    uint8 type; FBitReader_Serialize(bunch, &type, 1);   // advances Pos
    if (type == 0xC0) {
        // remainingBytes = (num - Pos) >> 3; read via FBitReader_Serialize
        // hand payload to the wire-transport receive (connKey = this->Connection@0x28)
        return;                                            // consume, skip original
    }
    *(int64*)(bunch + 0xA8) = savedPos;                    // rewind, not ours
}
original(this, bunch);
```

Getting the target `UControlChannel*`: capture it per `UNetConnection*` inside the
ReceivedBunch detour (every connection exchanges control traffic at join), keyed by
`this->Connection@0x28`. Server reaches a player's connection via
`UNetDriver::ClientConnections` / `APlayerController::NetConnection`; client via
`UNetDriver::ServerConnection`. Alternative: `UNetConnection::Channels[0]` is the
control channel (Channels array offset not yet pulled).

## Plugin wire table (ack tail and `$ML:wires`)

The loader's own messages ride the same wire as plugin packets, in envelopes
whose plugin name is `$ML` (`$ML:manifest`, `$ML:ack`, and the one below). By
default a plugin's packets flow only between identical version strings. A plugin
that exports `GetPluginWireInfo` (see `plugins/plugin_interface.h`) also tells
the loader its wire id and the versions it accepts, and the two loaders swap
those declarations in a **wire table**. The compatibility rule itself, and the
table codec, live in `wire_compat.h`.

Separators: records end with RS (0x1E), fields split on US (0x1F).

```
table  = "$wires" US "1" RS                       header: marker, table format
       + name US version US wireId US accept RS   one row per plugin
```

- `wireId` is a decimal number, or empty for a plugin that declared nothing.
  `accept` is the `;` separated version list, or empty.
- Limits: name and version 256 bytes each, accept list 512 bytes and 32 items, 512
  rows. A plugin whose fields hold a separator, whose name starts with `$`, or
  that exceeds a limit is left out of the table and logged once.
- A reader ignores rows with fewer than 4 fields, extra fields on a row, rows
  with a bad wire id or a name already seen, and records that start with `$` that
  it does not know. An unfinished last record (no RS) is dropped. A table without
  its header is no table, and the plugins in it keep the exact-version rule.

**Server to client: the tail of `$ML:ack`.** The ack stays
`buildTag US interfaceVersion RS`, and the table is appended after that first
record: one row for every initialized plugin on the server, cut after whole rows
at 8192 bytes (the cut is logged). Later record kinds may follow the table, each
starting with `$`. An older client reads the ack only up to the first RS, so it
never sees the table.

**Client to server: `$ML:wires`.** The payload is exactly the table above, with
no first record, carrying every plugin the client has loaded. The client sends it
once per manifest it puts on the wire, and only after an ack whose tail was a
readable table. That is the proof that the server's loader understands it: a
server whose ack has no tail is never sent `wires`, because an older server would
log an unknown `$ML` message for it. A manifest sent again (a plugin reloaded)
makes the server forget the client's table until `wires` arrives again; the
manifest record format itself is unchanged. A `wires` message with no manifest
before it is dropped.

Both loaders then evaluate every plugin against the other side's table with the
same rule, so each logs its own line when a pair is blocked or accepted.
