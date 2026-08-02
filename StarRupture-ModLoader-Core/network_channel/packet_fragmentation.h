#pragma once

// ============================================================
// packet_fragmentation.h
//
// Transport-agnostic chunking + reassembly for the plugin network channel.
//
// The plugin-facing API is byte-oriented: a plugin hands the modloader an
// (originalTag, payload) pair and expects the other end to receive exactly that
// pair back -- as one delivery, once every byte has arrived. The underlying
// transport (today: the FString envelope over ClientSaveStringToTxt /
// the UE control channel) has a finite per-message
// budget, so large payloads must be split on the way out and rebuilt on the way
// in. That split/rebuild is intentionally invisible to plugins -- they never see
// a partial payload.
//
// This module contains ONLY that logic. It has no dependency on the SDK, the
// logger, or Windows, so it can be unit-tested standalone. The engine-facing
// glue (which transport frame carries a chunk, which UNetConnection a chunk came
// from) lives in network_channel.cpp.
//
// Wire layout of a single chunk frame (all little-endian, tightly packed):
//
//   offset  size  field
//   0       4     magic          'M','F','R','G'  (0x4746524D LE)
//   4       1     version        kFragVersion
//   5       4     messageId      per-sender id, identifies one logical message
//   9       4     totalBytes     size of the fully reassembled payload
//   13      4     chunkOffset    byte offset of this chunk within the payload
//   17      2     chunkIndex     0-based index of this chunk
//   19      2     chunkCount     total number of chunks in the message
//   21      1     tagLen         length of originalTag (bytes, no null)
//   22      tagLen originalTag   ASCII plugin packet tag (repeated in every chunk)
//   ...     ...   chunkBytes     this chunk's slice of the payload
//
// chunkOffset is carried explicitly rather than reconstructed from index*size so
// the reassembler never needs to know the sender's chunk size -- it just checks
// the slice fits and writes it.
//
// Repeating messageId/totalBytes/chunkCount/originalTag in every chunk makes each
// frame self-describing: a receiver never depends on a prior "announcement" frame
// arriving first, and two in-flight messages from the same sender can interleave
// safely because each frame carries its own messageId.
// ============================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

namespace Fragmentation
{
    inline constexpr uint32_t kFragMagic   = 0x4746524Du; // 'MFRG' little-endian
    inline constexpr uint8_t  kFragVersion = 1;
    inline constexpr size_t   kFragHeaderBase = 22; // bytes before originalTag

    // Default safety limits. Callers may override on the Reassembler instance.
    inline constexpr uint32_t kDefaultMaxMessageBytes = 8u * 1024u * 1024u; // 8 MB
    inline constexpr uint16_t kDefaultMaxChunkCount   = 4096;
    inline constexpr uint64_t kDefaultTimeoutMs       = 15000; // drop stale partials

    // A completed, reassembled message handed back to the caller.
    struct Completed
    {
        std::string          originalTag;
        std::vector<uint8_t> payload;
    };

    // ----------------------------------------------------------------
    // BuildChunks -- split (originalTag, data) into one or more chunk frames.
    //
    //   messageId          : caller-chosen id, unique per in-flight message per
    //                        sender (a monotonically increasing counter is fine).
    //   maxChunkPayload    : max PAYLOAD bytes per frame (the transport's budget
    //                        minus this header's overhead). Must be >= 1.
    //
    // Returns a vector of ready-to-send frames. A zero-length payload still
    // produces exactly one (empty) chunk so the far side gets a delivery.
    // ----------------------------------------------------------------
    std::vector<std::vector<uint8_t>> BuildChunks(
        uint32_t messageId,
        const std::string& originalTag,
        const uint8_t* data,
        size_t size,
        size_t maxChunkPayload);

    // Reason a frame was rejected (for logging at the call site).
    enum class FeedResult
    {
        NeedMore,        // frame accepted, message not yet complete
        Completed,       // frame accepted and completed a message (out filled)
        NotAFragment,    // not a MFRG frame -- caller should treat it as a whole message
        Malformed,       // frame header failed validation; dropped
        LimitExceeded,   // declared size / chunk count over the configured cap; dropped
    };

    // ----------------------------------------------------------------
    // Reassembler -- accumulates chunk frames per connection and yields a
    // Completed message once all chunks of a messageId have arrived.
    //
    // connKey identifies the sending peer (server side: the sender
    // UNetConnection/PlayerController pointer as a uintptr_t; client side: 0,
    // since there is exactly one server). Reassembly buffers are scoped to
    // connKey so PurgeConnection() can drop everything for a peer that left.
    // ----------------------------------------------------------------
    class Reassembler
    {
    public:
        uint32_t maxMessageBytes = kDefaultMaxMessageBytes;
        uint16_t maxChunkCount   = kDefaultMaxChunkCount;
        uint64_t timeoutMs       = kDefaultTimeoutMs;

        // Feed one transport frame. nowMs is a monotonic millisecond clock used
        // for stale-partial expiry. On FeedResult::Completed, out is populated.
        FeedResult Feed(uintptr_t connKey,
                        const uint8_t* frame, size_t frameLen,
                        uint64_t nowMs,
                        Completed& out);

        // Drop every partial buffered for connKey (call on disconnect so a
        // reused connection pointer never inherits a stale half-message).
        void PurgeConnection(uintptr_t connKey);

        // Drop every partial older than timeoutMs. Returns how many were dropped.
        size_t ExpireStale(uint64_t nowMs);

        // Drop everything (shutdown).
        void Clear();

        // Total partial messages currently buffered (diagnostics/tests).
        size_t PartialCount() const;

    private:
        struct Partial
        {
            std::string          originalTag;
            uint32_t             totalBytes = 0;
            uint16_t             chunkCount = 0;
            uint16_t             received   = 0;
            uint64_t             lastMs     = 0;
            std::vector<uint8_t> buffer;      // sized to totalBytes
            std::vector<bool>    haveChunk;   // sized to chunkCount
        };

        // Partials are nested by connection so PurgeConnection is O(1) on the
        // outer key and reused pointers can never collide with a live peer's ids.
        std::unordered_map<uintptr_t,
            std::unordered_map<uint32_t, Partial>> m_partials;
    };

    // True if frame looks like a MFRG frame (magic + version + min length).
    bool IsFragmentFrame(const uint8_t* frame, size_t frameLen);

} // namespace Fragmentation
