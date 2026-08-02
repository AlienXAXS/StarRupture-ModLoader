#pragma once

// ============================================================
// plugin_packet.h
//
// A packet-definition API for plugins: describe a message once as a struct with
// Write/Read methods, then send it and receive it on the other end already
// decoded into an object -- no manual byte offsets, no fixed-size restriction.
//
// This sits ON TOP of the raw byte channel (IPluginNetworkChannel). Because it is
// byte-oriented, it transparently inherits the loader's fragmentation: a packet of
// any size is split and reassembled for you, and your Read callback only ever sees
// a complete object.
//
// Unlike Network::SendPacketToServer<T> in plugin_network_helpers.h (which requires
// T to be a fixed-size trivially-copyable POD and just memcpy's it), a Packet may
// contain variable-length fields -- strings, blobs, and arrays -- because it is
// explicitly serialized field by field.
//
// ---- Defining a packet ----
//
//   struct ChatBroadcast {
//       // A stable id you choose. Unique within your plugin is enough -- routing
//       // is already scoped per plugin. Never renumber a shipped packet.
//       static constexpr uint32_t kPacketId = 0x00C0FFEE;
//       static constexpr uint16_t kVersion  = 1;
//
//       int32_t                 senderId;
//       std::string             message;
//       std::vector<uint8_t>    iconBlob;
//
//       void Write(Packet::Writer& w) const {
//           w.I32(senderId);
//           w.Str(message);
//           w.Blob(iconBlob);
//       }
//       bool Read(Packet::Reader& r) {
//           r.I32(senderId);
//           r.Str(message);
//           r.Blob(iconBlob);
//           return r.Ok(); // false if the buffer was short/corrupt
//       }
//   };
//
// ---- Sending / receiving ----
//
//   // server:
//   Packet::SendToAllClients(hooks, self, ChatBroadcast{ id, msg, icon });
//   Packet::OnServerReceive<ChatBroadcast>(hooks, self,
//       [](void* senderPC, const ChatBroadcast& p){ ... });
//
//   // client:
//   Packet::SendToServer(hooks, self, ChatBroadcast{ id, msg, icon });
//   Packet::OnReceive<ChatBroadcast>(hooks, self,
//       [](const ChatBroadcast& p){ ... });
//
// Read is called on the game thread. A packet whose declared kPacketId or version
// does not match the receiving type is dropped before your callback runs.
// ============================================================

#include "plugin_interface.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace Packet
{

// ----------------------------------------------------------------
// Writer -- append fields little-endian into a growable buffer.
// ----------------------------------------------------------------
class Writer
{
public:
    void U8(uint8_t v)   { m_buf.push_back(v); }
    void U16(uint16_t v) { put(&v, sizeof v); }
    void U32(uint32_t v) { put(&v, sizeof v); }
    void U64(uint64_t v) { put(&v, sizeof v); }
    void I8(int8_t v)    { U8(static_cast<uint8_t>(v)); }
    void I16(int16_t v)  { U16(static_cast<uint16_t>(v)); }
    void I32(int32_t v)  { U32(static_cast<uint32_t>(v)); }
    void I64(int64_t v)  { U64(static_cast<uint64_t>(v)); }
    void F32(float v)    { put(&v, sizeof v); }
    void F64(double v)   { put(&v, sizeof v); }
    void Bool(bool v)    { U8(v ? 1 : 0); }

    // Length-prefixed (u32) UTF-8 string.
    void Str(const std::string& s)
    {
        U32(static_cast<uint32_t>(s.size()));
        put(s.data(), s.size());
    }

    // Length-prefixed (u32) raw byte blob.
    void Blob(const std::vector<uint8_t>& b)
    {
        U32(static_cast<uint32_t>(b.size()));
        put(b.data(), b.size());
    }

    // Length-prefixed (u32) array of trivially-copyable PODs.
    template<typename T>
    void Vec(const std::vector<T>& v)
    {
        static_assert(std::is_trivially_copyable_v<T>,
            "Packet::Writer::Vec<T> requires a trivially-copyable element type");
        U32(static_cast<uint32_t>(v.size()));
        if (!v.empty()) put(v.data(), v.size() * sizeof(T));
    }

    const std::vector<uint8_t>& Buffer() const { return m_buf; }

private:
    void put(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        m_buf.insert(m_buf.end(), b, b + n);
    }
    std::vector<uint8_t> m_buf;
};

// ----------------------------------------------------------------
// Reader -- bounds-checked sequential read. Any read past the end sets an
// error flag (Ok() == false) and yields zero/empty values, so a truncated or
// hostile buffer can never over-read.
// ----------------------------------------------------------------
class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : m_p(data), m_n(size) {}

    bool Ok() const     { return !m_err; }
    size_t Remaining() const { return m_err ? 0 : (m_n - m_pos); }

    uint8_t  U8()  { uint8_t v = 0;  get(&v, sizeof v); return v; }
    uint16_t U16() { uint16_t v = 0; get(&v, sizeof v); return v; }
    uint32_t U32() { uint32_t v = 0; get(&v, sizeof v); return v; }
    uint64_t U64() { uint64_t v = 0; get(&v, sizeof v); return v; }
    int8_t   I8()  { return static_cast<int8_t>(U8()); }
    int16_t  I16() { return static_cast<int16_t>(U16()); }
    int32_t  I32() { return static_cast<int32_t>(U32()); }
    int64_t  I64() { return static_cast<int64_t>(U64()); }
    float    F32() { float v = 0;  get(&v, sizeof v); return v; }
    double   F64() { double v = 0; get(&v, sizeof v); return v; }
    bool     Bool(){ return U8() != 0; }

    // Reference-style overloads so a Read() body can mirror the Write() body.
    void U8(uint8_t& v)   { v = U8(); }
    void U16(uint16_t& v) { v = U16(); }
    void U32(uint32_t& v) { v = U32(); }
    void U64(uint64_t& v) { v = U64(); }
    void I8(int8_t& v)    { v = I8(); }
    void I16(int16_t& v)  { v = I16(); }
    void I32(int32_t& v)  { v = I32(); }
    void I64(int64_t& v)  { v = I64(); }
    void F32(float& v)    { v = F32(); }
    void F64(double& v)   { v = F64(); }
    void Bool(bool& v)    { v = Bool(); }

    void Str(std::string& s)
    {
        const uint32_t n = U32();
        if (m_err || n > Remaining()) { fail(); s.clear(); return; }
        s.assign(reinterpret_cast<const char*>(m_p + m_pos), n);
        m_pos += n;
    }

    void Blob(std::vector<uint8_t>& b)
    {
        const uint32_t n = U32();
        if (m_err || n > Remaining()) { fail(); b.clear(); return; }
        b.assign(m_p + m_pos, m_p + m_pos + n);
        m_pos += n;
    }

    template<typename T>
    void Vec(std::vector<T>& v)
    {
        static_assert(std::is_trivially_copyable_v<T>,
            "Packet::Reader::Vec<T> requires a trivially-copyable element type");
        const uint32_t count = U32();
        const size_t bytes = static_cast<size_t>(count) * sizeof(T);
        if (m_err || bytes > Remaining()) { fail(); v.clear(); return; }
        v.resize(count);
        if (count) std::memcpy(v.data(), m_p + m_pos, bytes);
        m_pos += bytes;
    }

private:
    void get(void* out, size_t n)
    {
        if (m_err || n > Remaining()) { fail(); std::memset(out, 0, n); return; }
        std::memcpy(out, m_p + m_pos, n);
        m_pos += n;
    }
    void fail() { m_err = true; }

    const uint8_t* m_p;
    size_t m_n;
    size_t m_pos = 0;
    bool   m_err = false;
};

// ----------------------------------------------------------------
// Internal framing: [u32 packetId][u16 version][ body... ].
// The tag string used for channel routing is derived from the packet id so it is
// stable across builds and independent of compiler typeid naming.
// ----------------------------------------------------------------
namespace detail
{
    inline constexpr uint32_t kFrameMagic = 0x504B4C31u; // 'PKL1'

    template<typename T>
    inline std::string TagFor()
    {
        // "PKT:<hexId>" -- routing is already scoped per plugin, so the id alone
        // is unique enough; the prefix keeps it clear in logs.
        char buf[24];
        std::snprintf(buf, sizeof buf, "PKT:%08X", static_cast<unsigned>(T::kPacketId));
        return std::string(buf);
    }

    template<typename T>
    inline std::vector<uint8_t> Encode(const T& pkt)
    {
        Writer w;
        w.U32(kFrameMagic);
        w.U32(T::kPacketId);
        w.U16(T::kVersion);
        pkt.Write(w);
        return w.Buffer();
    }

    // Decode into out. Returns false (drop) if the frame header, id, or body is
    // wrong. Version mismatch is surfaced to the packet's own Read via the raw
    // fields already consumed; here we require an exact version match by default.
    template<typename T>
    inline bool Decode(const uint8_t* data, size_t size, T& out)
    {
        Reader r(data, size);
        if (r.U32() != kFrameMagic) return false;
        if (r.U32() != T::kPacketId) return false;
        if (r.U16() != T::kVersion) return false;
        if (!r.Ok()) return false;
        return out.Read(r);
    }
} // namespace detail

// ----------------------------------------------------------------
// Send helpers
// ----------------------------------------------------------------
template<typename T>
void SendToServer(IPluginHooks* hooks, const IPluginSelf* self, const T& pkt)
{
    if (!hooks || !hooks->Network) return;
    auto bytes = detail::Encode(pkt);
    hooks->Network->SendPacketToServer(self, detail::TagFor<T>().c_str(),
                                       bytes.data(), bytes.size());
}

template<typename T>
void SendToClient(IPluginHooks* hooks, const IPluginSelf* self,
                  void* playerController, const T& pkt)
{
    if (!hooks || !hooks->Network) return;
    auto bytes = detail::Encode(pkt);
    hooks->Network->SendPacketToClient(playerController, self, detail::TagFor<T>().c_str(),
                                       bytes.data(), bytes.size());
}

template<typename T>
void SendToAllClients(IPluginHooks* hooks, const IPluginSelf* self, const T& pkt)
{
    if (!hooks || !hooks->Network) return;
    auto bytes = detail::Encode(pkt);
    hooks->Network->SendPacketToAllClients(self, detail::TagFor<T>().c_str(),
                                           bytes.data(), bytes.size());
}

// ----------------------------------------------------------------
// Receive helpers. One active handler per packet type T per plugin (last call
// wins), matching the convention in plugin_network_helpers.h. Returns the raw
// callback pointer for later Unregister... during PluginShutdown.
// ----------------------------------------------------------------
template<typename T>
PluginNetworkMessageCallback OnReceive(IPluginHooks* hooks, const IPluginSelf* self,
                                       std::function<void(const T&)> cb)
{
    if (!hooks || !hooks->Network || !cb) return nullptr;

    struct Handler
    {
        static std::function<void(const T&)>& Callback()
        {
            static std::function<void(const T&)> s;
            return s;
        }
        static void Dispatch(const char* /*plugin*/, const char* /*tag*/,
                             const uint8_t* data, size_t size)
        {
            T pkt{};
            if (!detail::Decode(data, size, pkt)) return;
            auto& fn = Callback();
            if (fn) fn(pkt);
        }
    };

    Handler::Callback() = std::move(cb);
    hooks->Network->RegisterMessageHandler(self, detail::TagFor<T>().c_str(), &Handler::Dispatch);
    return &Handler::Dispatch;
}

template<typename T>
PluginNetworkServerMessageCallback OnServerReceive(
    IPluginHooks* hooks, const IPluginSelf* self,
    std::function<void(void* senderPC, const T&)> cb)
{
    if (!hooks || !hooks->Network || !cb) return nullptr;

    struct Handler
    {
        static std::function<void(void*, const T&)>& Callback()
        {
            static std::function<void(void*, const T&)> s;
            return s;
        }
        static void Dispatch(void* senderPC,
                             const char* /*plugin*/, const char* /*tag*/,
                             const uint8_t* data, size_t size)
        {
            T pkt{};
            if (!detail::Decode(data, size, pkt)) return;
            auto& fn = Callback();
            if (fn) fn(senderPC, pkt);
        }
    };

    Handler::Callback() = std::move(cb);
    hooks->Network->RegisterServerMessageHandler(self, detail::TagFor<T>().c_str(), &Handler::Dispatch);
    return &Handler::Dispatch;
}

} // namespace Packet
