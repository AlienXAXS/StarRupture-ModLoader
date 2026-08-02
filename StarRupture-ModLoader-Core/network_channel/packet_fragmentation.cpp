#include "packet_fragmentation.h"

// This module is pure logic (no SDK / logger / Windows) so it can be compiled and
// unit-tested on its own. pch.h is intentionally NOT included here.

namespace Fragmentation
{
    namespace
    {
        // Little-endian read/write helpers -- keep the wire layout explicit and
        // independent of the host's struct packing.
        void PutU32(std::vector<uint8_t>& v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x & 0xFF));
            v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
            v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
            v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
        }
        void PutU16(std::vector<uint8_t>& v, uint16_t x)
        {
            v.push_back(static_cast<uint8_t>(x & 0xFF));
            v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
        }
        uint32_t GetU32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }
        uint16_t GetU16(const uint8_t* p)
        {
            return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                   (static_cast<uint16_t>(p[1]) << 8));
        }
    } // namespace

    bool IsFragmentFrame(const uint8_t* frame, size_t frameLen)
    {
        if (!frame || frameLen < kFragHeaderBase) return false;
        if (GetU32(frame) != kFragMagic) return false;
        if (frame[4] != kFragVersion) return false;
        return true;
    }

    std::vector<std::vector<uint8_t>> BuildChunks(
        uint32_t messageId,
        const std::string& originalTag,
        const uint8_t* data,
        size_t size,
        size_t maxChunkPayload)
    {
        std::vector<std::vector<uint8_t>> out;

        if (maxChunkPayload < 1) maxChunkPayload = 1;

        // originalTag length is stored in a single byte.
        std::string tag = originalTag;
        if (tag.size() > 255) tag.resize(255);
        const uint8_t tagLen = static_cast<uint8_t>(tag.size());

        // Ceil-divide; a zero-length payload still yields one chunk.
        size_t chunkCount = (size == 0) ? 1 : (size + maxChunkPayload - 1) / maxChunkPayload;
        if (chunkCount == 0) chunkCount = 1;

        out.reserve(chunkCount);
        for (size_t i = 0; i < chunkCount; ++i)
        {
            const size_t start = i * maxChunkPayload;
            const size_t len   = (start < size)
                ? ((size - start < maxChunkPayload) ? (size - start) : maxChunkPayload)
                : 0;

            std::vector<uint8_t> frame;
            frame.reserve(kFragHeaderBase + tagLen + len);
            PutU32(frame, kFragMagic);
            frame.push_back(kFragVersion);
            PutU32(frame, messageId);
            PutU32(frame, static_cast<uint32_t>(size));
            PutU32(frame, static_cast<uint32_t>(start));
            PutU16(frame, static_cast<uint16_t>(i));
            PutU16(frame, static_cast<uint16_t>(chunkCount));
            frame.push_back(tagLen);
            frame.insert(frame.end(), tag.begin(), tag.end());
            if (len)
                frame.insert(frame.end(), data + start, data + start + len);

            out.push_back(std::move(frame));
        }
        return out;
    }

    FeedResult Reassembler::Feed(uintptr_t connKey,
                                 const uint8_t* frame, size_t frameLen,
                                 uint64_t nowMs,
                                 Completed& out)
    {
        if (!IsFragmentFrame(frame, frameLen))
            return FeedResult::NotAFragment;

        const uint32_t messageId   = GetU32(frame + 5);
        const uint32_t totalBytes  = GetU32(frame + 9);
        const uint32_t chunkOffset = GetU32(frame + 13);
        const uint16_t chunkIndex  = GetU16(frame + 17);
        const uint16_t chunkCount  = GetU16(frame + 19);
        const uint8_t  tagLen      = frame[21];

        // Header must physically fit.
        if (frameLen < kFragHeaderBase + tagLen)
            return FeedResult::Malformed;

        const uint8_t* tagPtr    = frame + kFragHeaderBase;
        const uint8_t* chunkPtr  = tagPtr + tagLen;
        const size_t   chunkLen  = frameLen - kFragHeaderBase - tagLen;

        // Structural sanity -- these "cannot happen" over a reliable ordered
        // transport, which is exactly why checking them catches a tampered peer.
        if (chunkCount == 0) return FeedResult::Malformed;
        if (chunkIndex >= chunkCount) return FeedResult::Malformed;

        // Caps -- validate BEFORE allocating so a forged header can't make us
        // reserve gigabytes.
        if (totalBytes > maxMessageBytes) return FeedResult::LimitExceeded;
        if (chunkCount > maxChunkCount)    return FeedResult::LimitExceeded;

        // Fast path: a single-chunk message that carries the whole payload.
        if (chunkCount == 1)
        {
            if (chunkLen != totalBytes) return FeedResult::Malformed;
            out.originalTag.assign(reinterpret_cast<const char*>(tagPtr), tagLen);
            out.payload.assign(chunkPtr, chunkPtr + chunkLen);
            return FeedResult::Completed;
        }

        auto& byConn = m_partials[connKey];
        auto  it     = byConn.find(messageId);
        if (it == byConn.end())
        {
            Partial p;
            p.originalTag.assign(reinterpret_cast<const char*>(tagPtr), tagLen);
            p.totalBytes = totalBytes;
            p.chunkCount = chunkCount;
            p.received   = 0;
            p.lastMs     = nowMs;
            p.buffer.assign(totalBytes, 0);
            p.haveChunk.assign(chunkCount, false);
            it = byConn.emplace(messageId, std::move(p)).first;
        }

        Partial& p = it->second;

        // Every chunk of a message must agree on the framing.
        if (p.totalBytes != totalBytes || p.chunkCount != chunkCount)
        {
            byConn.erase(it);
            return FeedResult::Malformed;
        }

        // The frame carries its own byte offset, so validation is a plain bounds
        // check -- no assumption about the sender's chunk size. An empty chunk is
        // only legal for a zero-length payload (which takes the single-chunk fast
        // path above), so a non-final multi-chunk frame must carry bytes.
        const size_t offset = chunkOffset;
        if (offset > p.totalBytes ||
            offset + chunkLen > p.totalBytes ||
            chunkLen == 0)
        {
            byConn.erase(it);
            return FeedResult::Malformed;
        }

        p.lastMs = nowMs;
        if (!p.haveChunk[chunkIndex])
        {
            std::memcpy(p.buffer.data() + offset, chunkPtr, chunkLen);
            p.haveChunk[chunkIndex] = true;
            ++p.received;
        }
        // Duplicate index (reliable transport shouldn't produce one) is ignored.

        if (p.received == p.chunkCount)
        {
            out.originalTag = std::move(p.originalTag);
            out.payload     = std::move(p.buffer);
            byConn.erase(it);
            if (byConn.empty()) m_partials.erase(connKey);
            return FeedResult::Completed;
        }

        return FeedResult::NeedMore;
    }

    void Reassembler::PurgeConnection(uintptr_t connKey)
    {
        m_partials.erase(connKey);
    }

    size_t Reassembler::ExpireStale(uint64_t nowMs)
    {
        size_t dropped = 0;
        for (auto connIt = m_partials.begin(); connIt != m_partials.end();)
        {
            auto& byMsg = connIt->second;
            for (auto it = byMsg.begin(); it != byMsg.end();)
            {
                // Guard against clock going backwards.
                const uint64_t age = (nowMs >= it->second.lastMs)
                    ? (nowMs - it->second.lastMs) : 0;
                if (age >= timeoutMs)
                {
                    it = byMsg.erase(it);
                    ++dropped;
                }
                else
                {
                    ++it;
                }
            }
            if (byMsg.empty()) connIt = m_partials.erase(connIt);
            else               ++connIt;
        }
        return dropped;
    }

    void Reassembler::Clear()
    {
        m_partials.clear();
    }

    size_t Reassembler::PartialCount() const
    {
        size_t n = 0;
        for (const auto& kv : m_partials) n += kv.second.size();
        return n;
    }

} // namespace Fragmentation
