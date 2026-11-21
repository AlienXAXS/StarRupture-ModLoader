#pragma once

// Plugin packet compatibility between a client and a server loader.
//
// By default a plugin's packets flow only when the client's and the server's
// version strings are equal. A plugin can opt in to more by exporting
// GetPluginWireInfo (see plugin_interface.h): a wire id for its packet layout
// and a list of older versions whose layout it still speaks. The two loaders
// swap those declarations (the server's table rides on the manifest ack, the
// client's in a "wires" message) and apply the one rule below on both sides.
//
// Everything here is pure: no globals, no logging, no game or Windows
// includes, so a plain host test compiles it. Callers lock around WarnOnce.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace WireCompat
{
    // Control characters keep the table clear of anything a plugin name or
    // version can hold. Records end with kRecordSep, fields split on kFieldSep.
    constexpr char kRecordSep = '\x1e';
    constexpr char kFieldSep = '\x1f';
    constexpr char kListSep = ';';

    // Marker and format version of the first record of a wire table.
    constexpr const char* kTableMarker = "$wires";
    constexpr uint32_t kTableVersion = 1;

    // Limits that keep a hostile or broken peer from costing memory.
    constexpr size_t kMaxFieldLen = 256;    // name, version
    constexpr size_t kMaxAcceptLen = 512;   // the whole accept list
    constexpr size_t kMaxAcceptItems = 32;
    constexpr size_t kMaxEntries = 512;

    // Returned by FindTail when the data has no record separator.
    constexpr size_t kNoTail = static_cast<size_t>(-1);

    // What a plugin declared. Not declared means exact version strings only.
    // A wire id of 0 means "no wire id": the accept list still counts, but
    // two zeros never make a match.
    struct Decl
    {
        bool declared = false;
        uint32_t wireId = 0;
        std::string accept; // ';' separated, normalized by MakeDecl
    };

    // One plugin as one side knows it.
    struct Entry
    {
        std::string name;
        std::string version;
        Decl decl;
    };

    enum class Verdict
    {
        NameMismatch,     // not the same plugin: never matched by name alone
        SameVersion,      // version strings are equal
        WireIdEqual,      // both declared, same non-zero wire id
        ServerListsClient, // server's accept list names the client's version
        ClientListsServer, // client's accept list names the server's version
        Incompatible
    };

    enum class PairState
    {
        Ready,        // packets may flow
        NotReported,  // the other side has no plugin of that name
        Incompatible  // same plugin, versions not known to share a wire format
    };

    namespace detail
    {
        inline bool IsSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        inline bool IsControl(char c)
        {
            return static_cast<unsigned char>(c) < 0x20;
        }

        inline std::string Trim(const std::string& s)
        {
            size_t b = 0;
            size_t e = s.size();
            while (b < e && IsSpace(s[b]))
                ++b;
            while (e > b && IsSpace(s[e - 1]))
                --e;
            return s.substr(b, e - b);
        }

        // Splits on sep and keeps empty fields.
        inline std::vector<std::string> Split(const std::string& s, char sep)
        {
            std::vector<std::string> out;
            size_t start = 0;
            for (;;)
            {
                const size_t at = s.find(sep, start);
                if (at == std::string::npos)
                {
                    out.push_back(s.substr(start));
                    break;
                }
                out.push_back(s.substr(start, at - start));
                start = at + 1;
            }
            return out;
        }

        inline bool ParseUint32(const std::string& s, uint32_t* out)
        {
            if (s.empty() || s.size() > 10)
                return false;
            uint64_t v = 0;
            for (char c : s)
            {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10 + static_cast<uint64_t>(c - '0');
            }
            if (v > 0xFFFFFFFFull)
                return false;
            *out = static_cast<uint32_t>(v);
            return true;
        }

        // A name or version that can travel in a record.
        inline bool FieldOk(const std::string& s)
        {
            if (s.size() > kMaxFieldLen)
                return false;
            return std::none_of(s.begin(), s.end(), [](char c) { return c == kRecordSep || c == kFieldSep; });
        }
    }

    // The ';' list as its elements: trimmed, empty and control-character
    // elements dropped, duplicates removed, at most kMaxAcceptItems. A list
    // longer than kMaxAcceptLen is treated as empty. Never throws.
    inline std::vector<std::string> ParseAcceptList(const std::string& list)
    {
        std::vector<std::string> out;
        if (list.size() > kMaxAcceptLen)
            return out;
        for (const std::string& raw : detail::Split(list, kListSep))
        {
            const std::string item = detail::Trim(raw);
            if (item.empty() || item.size() > kMaxFieldLen)
                continue;
            if (std::any_of(item.begin(), item.end(), detail::IsControl))
                continue;
            if (std::find(out.begin(), out.end(), item) != out.end())
                continue;
            out.push_back(item);
            if (out.size() >= kMaxAcceptItems)
                break;
        }
        return out;
    }

    // Exact element match, never a substring: "1.0.1" does not list "1.0".
    inline bool AcceptListContains(const std::string& list, const std::string& version)
    {
        if (version.empty())
            return false;
        const std::vector<std::string> items = ParseAcceptList(list);
        return std::find(items.begin(), items.end(), version) != items.end();
    }

    // Builds the declaration the loader keeps for a plugin that exports
    // GetPluginWireInfo. The accept list is normalized (parsed and joined with
    // ';'). A list that cannot travel (a control character in it, or longer
    // than kMaxAcceptLen) is dropped to empty and reported through
    // acceptRejected so the caller can warn; the wire id still counts.
    inline Decl MakeDecl(uint32_t wireId, const char* accept, bool* acceptRejected = nullptr)
    {
        Decl d;
        d.declared = true;
        d.wireId = wireId;
        if (acceptRejected)
            *acceptRejected = false;
        if (!accept || !*accept)
            return d;

        const std::string raw(accept);
        const bool bad = raw.size() > kMaxAcceptLen
            || std::any_of(raw.begin(), raw.end(), [](char c) { return c == kRecordSep || c == kFieldSep; });
        if (bad)
        {
            if (acceptRejected)
                *acceptRejected = true;
            return d;
        }
        const std::vector<std::string> items = ParseAcceptList(raw);
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i)
                d.accept += kListSep;
            d.accept += items[i];
        }
        return d;
    }

    // Does the client's plugin speak the same wire format as the server's?
    // Order: same plugin name (never name alone), equal version strings, equal
    // wire id on two declared sides, the server's list names the client's
    // version, the client's list names the server's version. A side that did
    // not declare contributes nothing but its version string.
    inline Verdict Evaluate(const Entry& server, const Entry& client)
    {
        if (server.name != client.name)
            return Verdict::NameMismatch;
        if (server.version == client.version)
            return Verdict::SameVersion;
        if (server.decl.declared && client.decl.declared
            && server.decl.wireId != 0 && server.decl.wireId == client.decl.wireId)
            return Verdict::WireIdEqual;
        if (server.decl.declared && AcceptListContains(server.decl.accept, client.version))
            return Verdict::ServerListsClient;
        if (client.decl.declared && AcceptListContains(client.decl.accept, server.version))
            return Verdict::ClientListsServer;
        return Verdict::Incompatible;
    }

    inline bool IsCompatible(Verdict v)
    {
        return v != Verdict::NameMismatch && v != Verdict::Incompatible;
    }

    // For the log line that notes a pair with different version strings that
    // is accepted. Empty for the verdicts that need no explanation.
    inline const char* Reason(Verdict v)
    {
        switch (v)
        {
        case Verdict::WireIdEqual:
            return "same wire id on both sides";
        case Verdict::ServerListsClient:
            return "listed as compatible by the server plugin";
        case Verdict::ClientListsServer:
            return "listed as compatible by the client plugin";
        default:
            return "";
        }
    }

    // "none" or the number, for the warning that names both wire ids.
    inline std::string WireIdText(const Decl& d)
    {
        return d.declared ? std::to_string(d.wireId) : std::string("none");
    }

    inline const Entry* FindEntry(const std::vector<Entry>& table, const std::string& name)
    {
        for (const Entry& e : table)
        {
            if (e.name == name)
                return &e;
        }
        return nullptr;
    }

    // The one decision for a plugin on one connection, shared by the send
    // paths, the ready check and the receive guard. other is the peer's entry
    // for that plugin (null when it reported none); local is this side's.
    // The server is always the first argument of Evaluate, so pass
    // serverIsLocal to say which one this side is. why, when given, gets the
    // verdict.
    inline PairState ClassifyPair(const Entry* other, const Entry& local, bool serverIsLocal, Verdict* why = nullptr)
    {
        if (!other)
        {
            if (why)
                *why = Verdict::NameMismatch;
            return PairState::NotReported;
        }
        const Verdict v = serverIsLocal ? Evaluate(local, *other) : Evaluate(*other, local);
        if (why)
            *why = v;
        return IsCompatible(v) ? PairState::Ready : PairState::Incompatible;
    }

    // " (wire id N on the server, M on the client)" for a warning when both sides
    // declared, empty otherwise.
    inline std::string WireIdsNote(const Decl& server, const Decl& client)
    {
        if (!server.declared || !client.declared)
            return std::string();
        return " (wire id " + WireIdText(server) + " on the server, " + WireIdText(client) + " on the client)";
    }

    // A local plugin that also runs on the server with a different version
    // string, and what the shared rule says about the pair.
    struct MirrorFinding
    {
        Entry local;
        Entry server;
        PairState state = PairState::Ready;
        Verdict why = Verdict::SameVersion;
    };

    // The client's view of the server's table: one finding for every local
    // plugin the server also lists whose version string differs. Plugins with
    // equal versions, or that the server does not list, are not findings.
    inline std::vector<MirrorFinding> MirrorCheck(const std::vector<Entry>& local, const std::vector<Entry>& serverTable)
    {
        std::vector<MirrorFinding> out;
        for (const Entry& l : local)
        {
            const Entry* s = FindEntry(serverTable, l.name);
            if (!s || s->version == l.version)
                continue;
            MirrorFinding f;
            f.local = l;
            f.server = *s;
            f.state = ClassifyPair(s, l, false, &f.why);
            out.push_back(std::move(f));
        }
        return out;
    }

    // "Name 1.2.3, Other 4.5" for the local plugins named in names (sorted, each
    // once). A name with no local entry is left out.
    inline std::string DescribePlugins(const std::vector<std::string>& names, const std::vector<Entry>& local)
    {
        std::set<std::string> sorted(names.begin(), names.end());
        std::string out;
        for (const std::string& n : sorted)
        {
            const Entry* e = FindEntry(local, n);
            if (!e)
                continue;
            if (!out.empty())
                out += ", ";
            out += e->name + " " + e->version;
        }
        return out;
    }

    // The wire table: records ending in kRecordSep, fields split on kFieldSep.
    //   $wires US 1 RS
    //   name US version US wireId-or-empty US accept RS   (one per plugin)
    // An empty wire id field means not declared. Names starting with '$' are
    // reserved for future record types.
    //
    // An ack tail is a run of records. The declaration table starts at its
    // $wires header and its rows are the only records that do not start with
    // '$'. Any other kind of record starts with '$' and follows the table; a
    // reader skips a '$' record it does not know.
    inline bool EntryEncodable(const Entry& e)
    {
        return !e.name.empty() && e.name[0] != '$' && detail::FieldOk(e.name) && detail::FieldOk(e.version)
            && e.decl.accept.size() <= kMaxAcceptLen
            && std::none_of(e.decl.accept.begin(), e.decl.accept.end(),
                [](char c) { return c == kRecordSep || c == kFieldSep; });
    }

    // Encodes the header and every encodable entry. Entries that cannot travel
    // (a separator character, a '$' name, an oversized field) are left out and
    // their names go to dropped, so the call site can warn like it does for the
    // manifest. The result stays within maxBytes when it can: the header is
    // always kept, entries go in whole, and the first one that would not fit
    // ends the table. Entries cut that way are counted in truncated.
    inline std::string EncodeTableCapped(const std::vector<Entry>& entries, size_t maxBytes,
        std::vector<std::string>* dropped = nullptr, size_t* truncated = nullptr)
    {
        std::string out = kTableMarker;
        out += kFieldSep;
        out += std::to_string(kTableVersion);
        out += kRecordSep;
        if (truncated)
            *truncated = 0;
        size_t written = 0;
        bool full = false;
        for (const Entry& e : entries)
        {
            if (!EntryEncodable(e) || written >= kMaxEntries)
            {
                if (dropped)
                    dropped->push_back(e.name);
                continue;
            }
            std::string record = e.name;
            record += kFieldSep;
            record += e.version;
            record += kFieldSep;
            if (e.decl.declared)
                record += std::to_string(e.decl.wireId);
            record += kFieldSep;
            if (e.decl.declared)
                record += e.decl.accept;
            record += kRecordSep;
            if (full || maxBytes < out.size() || record.size() > maxBytes - out.size())
            {
                full = true;
                if (truncated)
                    ++*truncated;
                continue;
            }
            out += record;
            ++written;
        }
        return out;
    }

    inline std::string EncodeTable(const std::vector<Entry>& entries, std::vector<std::string>* dropped = nullptr)
    {
        return EncodeTableCapped(entries, static_cast<size_t>(-1), dropped, nullptr);
    }

    // Offset just after the first record separator, which is where a wire table
    // starts when it follows the ack's first record. kNoTail when there is no
    // separator. An offset equal to len is an empty tail. Older clients read
    // the ack only up to that first separator.
    inline size_t FindTail(const char* data, size_t len)
    {
        if (!data)
            return kNoTail;
        for (size_t i = 0; i < len; ++i)
        {
            if (data[i] == kRecordSep)
                return i + 1;
        }
        return kNoTail;
    }

    struct DecodedTable
    {
        bool present = false; // the header was there; entries may still be empty
        std::vector<Entry> entries;
    };

    // Reads a table that starts with the header record. Without the header
    // present stays false (an old peer). Records that are short, malformed,
    // unknown or duplicated by name are skipped, extra fields are ignored and
    // an incomplete last record (no separator) is dropped. Never throws.
    inline DecodedTable DecodeTable(const char* data, size_t len)
    {
        DecodedTable out;
        if (!data || len == 0)
            return out;

        const std::string all(data, len);
        const size_t lastRs = all.rfind(kRecordSep);
        if (lastRs == std::string::npos)
            return out;
        const std::string complete = all.substr(0, lastRs); // drops the unfinished tail
        bool first = true;
        for (const std::string& record : detail::Split(complete, kRecordSep))
        {
            const std::vector<std::string> f = detail::Split(record, kFieldSep);
            if (first)
            {
                first = false;
                uint32_t version = 0;
                if (f.size() < 2 || f[0] != kTableMarker || !detail::ParseUint32(f[1], &version) || version < 1)
                    return out;
                out.present = true;
                continue;
            }
            if (f.size() < 4 || f[0].empty() || f[0][0] == '$' || !detail::FieldOk(f[0]) || !detail::FieldOk(f[1]))
                continue;
            if (f[3].size() > kMaxAcceptLen)
                continue;
            if (out.entries.size() >= kMaxEntries)
                break;
            if (FindEntry(out.entries, f[0]))
                continue;

            Entry e;
            e.name = f[0];
            e.version = f[1];
            if (!f[2].empty())
            {
                uint32_t id = 0;
                if (!detail::ParseUint32(f[2], &id))
                    continue;
                e.decl.declared = true;
                e.decl.wireId = id;
                e.decl.accept = f[3];
            }
            out.entries.push_back(std::move(e));
        }
        return out;
    }

    // The table behind the first record of an ack, or an absent table when the
    // ack has no tail (an old server).
    inline DecodedTable DecodeAckTail(const char* data, size_t len)
    {
        const size_t at = FindTail(data, len);
        if (at == kNoTail || at >= len)
            return DecodedTable{};
        return DecodeTable(data + at, len - at);
    }

    // What follows the first record of an ack: nothing (an old server), bytes
    // that are not a readable table, or a table. Only a table proves the
    // server's loader understands the wire declarations.
    enum class AckTailKind
    {
        None,
        Garbage,
        Table,
    };

    // table, when given, receives the decoded table for AckTailKind::Table.
    inline AckTailKind ReadAckTail(const char* data, size_t len, DecodedTable* table = nullptr)
    {
        const size_t at = FindTail(data, len);
        if (at == kNoTail || at >= len)
            return AckTailKind::None;
        DecodedTable t = DecodeTable(data + at, len - at);
        if (!t.present)
            return AckTailKind::Garbage;
        if (table)
            *table = std::move(t);
        return AckTailKind::Table;
    }

    // The host rules record: "$hostrules US 1 RS", a tail record behind the
    // wire table. A server whose loader reads and applies host rules always
    // writes it, so a client that finds it knows the server understands
    // "$ML:feat" and sends it. A reader that does not know the record skips it
    // like any other '$' record.
    constexpr const char* kHostRulesMarker = "$hostrules";
    constexpr uint32_t kHostRulesVersion = 1;

    inline std::string EncodeHostRulesRecord()
    {
        std::string out = kHostRulesMarker;
        out += kFieldSep;
        out += std::to_string(kHostRulesVersion);
        out += kRecordSep;
        return out;
    }

    // True when a complete "$hostrules" record of version 1 or later follows
    // the first record of an ack. An unfinished last record does not count.
    inline bool TailHasHostRules(const char* data, size_t len)
    {
        const size_t at = FindTail(data, len);
        if (at == kNoTail || at >= len)
            return false;
        const std::string all(data + at, len - at);
        const size_t lastRs = all.rfind(kRecordSep);
        if (lastRs == std::string::npos)
            return false;
        for (const std::string& record : detail::Split(all.substr(0, lastRs), kRecordSep))
        {
            const std::vector<std::string> f = detail::Split(record, kFieldSep);
            uint32_t version = 0;
            if (f.size() >= 2 && f[0] == kHostRulesMarker && detail::ParseUint32(f[1], &version) && version >= 1)
                return true;
        }
        return false;
    }

    // Remembers what was already reported so a warning or note about a plugin
    // on a connection is logged once. Keyed by connection and plugin name; the
    // connection is only an identity (never dereferenced). The caller locks.
    class WarnOnce
    {
    public:
        // True the first time this connection and plugin are seen.
        bool First(const void* conn, const std::string& plugin)
        {
            return m_seen.insert(std::make_pair(conn, plugin)).second;
        }

        // Forgets one connection's keys and no other's.
        void Forget(const void* conn)
        {
            for (auto it = m_seen.begin(); it != m_seen.end();)
            {
                if (it->first == conn)
                    it = m_seen.erase(it);
                else
                    ++it;
            }
        }

        void Clear() { m_seen.clear(); }

        size_t Size() const { return m_seen.size(); }

    private:
        std::set<std::pair<const void*, std::string>> m_seen;
    };
}
