#pragma once

// What a server and a client say to each other about host rules, as bytes.
//
// Two loader-internal messages ride the "$ML" envelope (the same way the
// manifest and the wire table do):
//
//   $ML:feat   client -> server. Who the client is and which plugin features it
//              declares, with whether each is on for this game build.
//   $ML:rules  server -> client. The server's rule set: a default, a note for
//              the player and one row per feature or mod it names.
//
// Both payloads are records ending in kRecordSep, fields split on kFieldSep,
// the same separators as the wire table (wire_compat.h), so a plugin name or a
// title can hold any printable text. A field never holds a control character.
//
//   $ML:feat payload
//     $loader US <build tag> US <interface version> RS          (first record)
//     <plugin> US <feature id> US <title> US <1 on | 0 off by build> RS
//
//   $ML:rules payload
//     $rules US 1 US <default 1 allow | 0 deny> US <on-denied mode> US <message> RS
//     <plugin> US <feature id | *> US <1 allow | 0 deny> RS
//
// A "*" feature id names the whole mod. A plugin name starting with '$' is
// reserved for record kinds added later: a reader skips such a row, and a
// writer never sends a plugin by that name. The on-denied mode is 0 (switch the feature off); it is
// carried as sent and the reader decides what a value it does not know means.
//
// Everything here is pure, like wire_compat.h: no globals, no logging, no game
// or Windows includes, so a plain host test compiles it. A decode never reads
// outside the buffer it is given and never throws; it either returns Ok with
// the whole payload or an error status and leaves the output untouched.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wire_compat.h"
#include "packet_fragmentation.h"
#include "plugins/host_rules.h"
#include "auto_update/version_compare.h"

namespace HostRulesWire
{
    constexpr char kRecordSep = WireCompat::kRecordSep;
    constexpr char kFieldSep = WireCompat::kFieldSep;

    // Type tags under the "$ML" plugin name, as BuildEnvelope takes them.
    constexpr const char* kTagFeat = "feat";
    constexpr const char* kTagRules = "rules";

    constexpr const char* kLoaderMarker = "$loader";
    constexpr const char* kRulesMarker = "$rules";
    constexpr uint32_t kRulesVersion = 1;

    // The feature id that names a whole mod in a rules row.
    constexpr const char* kWholePlugin = "*";

    // The only on-denied mode so far: the denied feature is switched off.
    constexpr uint8_t kOnDeniedSwitchOff = 0;

    // Limits that keep a hostile or broken peer from costing memory.
    constexpr size_t kMaxPayloadBytes = 256 * 1024;
    constexpr size_t kMaxRecords = 2048; // the header record counts
    constexpr size_t kMaxFieldsPerRecord = 8;
    constexpr size_t kMaxBuildTagLen = 64;
    constexpr size_t kMaxPluginLen = 128;
    constexpr size_t kMaxIdLen = 128;
    constexpr size_t kMaxTitleLen = 256;
    constexpr size_t kMaxMessageLen = 200;

    enum class Status
    {
        Ok,
        Empty,          // no bytes
        TooLarge,       // over kMaxPayloadBytes
        Truncated,      // the last record has no separator: the data was cut
        TooManyRecords, // over kMaxRecords
        BadHeader,      // first record missing, wrong marker or unreadable
        BadRecord,      // a row with too few or too many fields or a bad value
        FieldTooLong,   // a field over its limit
        ControlChar,    // a control character inside a field
    };

    inline const char* StatusText(Status s)
    {
        switch (s)
        {
        case Status::Ok: return "ok";
        case Status::Empty: return "empty";
        case Status::TooLarge: return "too large";
        case Status::Truncated: return "truncated";
        case Status::TooManyRecords: return "too many records";
        case Status::BadHeader: return "bad header";
        case Status::BadRecord: return "bad record";
        case Status::FieldTooLong: return "field too long";
        case Status::ControlChar: return "control character";
        }
        return "unknown";
    }

    // What a writer left out. Rows that cannot travel are dropped; rows that
    // would pass the record or size limit are cut off at the first one.
    struct EncodeStats
    {
        size_t dropped = 0;
        size_t truncated = 0;
    };

    namespace detail
    {
        inline bool IsCtl(char c)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            return u < 0x20 || u == 0x7F;
        }

        inline bool HasCtl(std::string_view s)
        {
            for (char c : s)
                if (IsCtl(c))
                    return true;
            return false;
        }

        inline bool ParseUint(std::string_view s, uint32_t* out)
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

        // "1" or "0", nothing else.
        inline bool ParseFlag(std::string_view s, bool* out)
        {
            if (s.size() != 1 || (s[0] != '0' && s[0] != '1'))
                return false;
            *out = s[0] == '1';
            return true;
        }

        using Fields = std::vector<std::string_view>;

        // Cuts data into records and records into fields. The views point into
        // data. Checks the shape (size, separators, record and field counts,
        // control characters) and nothing about meaning.
        inline Status SplitRecords(const char* data, size_t len, std::vector<Fields>* out)
        {
            if (!data || len == 0)
                return Status::Empty;
            if (len > kMaxPayloadBytes)
                return Status::TooLarge;
            if (data[len - 1] != kRecordSep)
                return Status::Truncated;

            size_t pos = 0;
            while (pos < len)
            {
                size_t end = pos;
                while (data[end] != kRecordSep) // the last byte is a separator
                    ++end;
                if (out->size() >= kMaxRecords)
                    return Status::TooManyRecords;

                Fields fields;
                size_t start = pos;
                for (size_t i = pos; i <= end; ++i)
                {
                    if (i == end || data[i] == kFieldSep)
                    {
                        if (fields.size() >= kMaxFieldsPerRecord)
                            return Status::BadRecord;
                        const std::string_view f(data + start, i - start);
                        if (HasCtl(f))
                            return Status::ControlChar;
                        fields.push_back(f);
                        start = i + 1;
                    }
                }
                out->push_back(std::move(fields));
                pos = end + 1;
            }
            return Status::Ok;
        }

        inline bool PluginFieldOk(const std::string& s)
        {
            return !s.empty() && s[0] != '$' && s.size() <= kMaxPluginLen && !HasCtl(s);
        }

        inline bool IdFieldOk(const std::string& s)
        {
            return !s.empty() && s.size() <= kMaxIdLen && !HasCtl(s);
        }

        inline void AppendFlag(std::string& out, bool v)
        {
            out += v ? '1' : '0';
        }
    }

    // A note or a title made safe to send: control characters become spaces,
    // the ends are trimmed and the text is cut to maxLen bytes without splitting
    // a UTF-8 character.
    inline std::string SanitizeText(const std::string& in, size_t maxLen)
    {
        std::string s = in;
        for (char& c : s)
            if (detail::IsCtl(c))
                c = ' ';
        size_t b = 0;
        size_t e = s.size();
        while (b < e && s[b] == ' ')
            ++b;
        while (e > b && s[e - 1] == ' ')
            --e;
        s = s.substr(b, e - b);
        if (s.size() > maxLen)
        {
            size_t n = maxLen;
            while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
                --n;
            s.resize(n);
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
        }
        return s;
    }

    // ---------------------------------------------------------------- $ML:feat

    struct FeatRecord
    {
        std::string plugin;
        std::string id;
        std::string title;
        bool on = false; // on for this game build; the server's rules come on top
    };

    struct FeatPayload
    {
        std::string buildTag;          // the client loader's build tag
        uint32_t interfaceVersion = 0; // its plugin interface version
        std::vector<FeatRecord> features;
    };

    // The record a feature needs to travel: a plugin and an id that are safe
    // and not reserved. The title is made safe by the writer instead.
    inline bool FeatEncodable(const FeatRecord& f)
    {
        return detail::PluginFieldOk(f.plugin) && detail::IdFieldOk(f.id) && f.id != kWholePlugin;
    }

    // The payload for $ML:feat. A build tag that cannot travel goes out as
    // "dev", which the minimum check reads as unparseable and never acts on.
    // Features that cannot travel are counted in stats->dropped; features past
    // the record or size limit in stats->truncated.
    inline std::string EncodeFeat(const FeatPayload& p, EncodeStats* stats = nullptr)
    {
        if (stats)
            *stats = EncodeStats{};
        std::string out = kLoaderMarker;
        out += kFieldSep;
        const bool tagOk = !p.buildTag.empty() && p.buildTag.size() <= kMaxBuildTagLen && !detail::HasCtl(p.buildTag);
        out += tagOk ? p.buildTag : std::string("dev");
        out += kFieldSep;
        out += std::to_string(p.interfaceVersion);
        out += kRecordSep;

        size_t records = 1;
        bool full = false;
        for (const FeatRecord& f : p.features)
        {
            if (!FeatEncodable(f))
            {
                if (stats)
                    ++stats->dropped;
                continue;
            }
            std::string rec = f.plugin;
            rec += kFieldSep;
            rec += f.id;
            rec += kFieldSep;
            rec += SanitizeText(f.title, kMaxTitleLen);
            rec += kFieldSep;
            detail::AppendFlag(rec, f.on);
            rec += kRecordSep;
            if (full || records >= kMaxRecords || out.size() + rec.size() > kMaxPayloadBytes)
            {
                full = true;
                if (stats)
                    ++stats->truncated;
                continue;
            }
            out += rec;
            ++records;
        }
        return out;
    }

    // Reads a $ML:feat payload. Strict: any row that is not well formed ends
    // the read with an error and *out stays as it was. A repeat of a plugin and
    // feature id already seen is skipped, and a row whose plugin name starts
    // with '$' is skipped as a record kind this reader does not know.
    inline Status DecodeFeat(const char* data, size_t len, FeatPayload* out)
    {
        std::vector<detail::Fields> recs;
        const Status st = detail::SplitRecords(data, len, &recs);
        if (st != Status::Ok)
            return st;

        const detail::Fields& h = recs[0];
        uint32_t iface = 0;
        if (h.size() < 3 || h[0] != kLoaderMarker || h[1].empty() || !detail::ParseUint(h[2], &iface))
            return Status::BadHeader;
        if (h[1].size() > kMaxBuildTagLen)
            return Status::FieldTooLong;

        FeatPayload p;
        p.buildTag.assign(h[1]);
        p.interfaceVersion = iface;
        for (size_t i = 1; i < recs.size(); ++i)
        {
            const detail::Fields& r = recs[i];
            if (!r[0].empty() && r[0][0] == '$')
                continue;
            bool on = false;
            if (r.size() < 4 || r[0].empty() || r[1].empty() || r[1] == kWholePlugin || !detail::ParseFlag(r[3], &on))
                return Status::BadRecord;
            if (r[0].size() > kMaxPluginLen || r[1].size() > kMaxIdLen || r[2].size() > kMaxTitleLen)
                return Status::FieldTooLong;

            FeatRecord f;
            f.plugin.assign(r[0]);
            f.id.assign(r[1]);
            f.title.assign(r[2]);
            f.on = on;
            bool seen = false;
            for (const FeatRecord& g : p.features)
                if (g.id == f.id && HostRules::SamePlugin(g.plugin, f.plugin))
                {
                    seen = true;
                    break;
                }
            if (!seen)
                p.features.push_back(std::move(f));
        }
        *out = std::move(p);
        return Status::Ok;
    }

    // --------------------------------------------------------------- $ML:rules

    // The server's rule set. A deny row names a feature (or, with id "*", a
    // whole mod) to switch off; an allow row names one to leave on although the
    // default or a wider row denies it. Working out which row wins for a given
    // feature (feature row, then whole-mod row, then default) is the caller's.
    struct RulesSet
    {
        bool defaultAllow = true;
        uint8_t onDenied = kOnDeniedSwitchOff;
        std::string message; // plain text for the player, at most kMaxMessageLen bytes
        HostRules::DenySet deny;
        HostRules::DenySet allow;
    };

    inline bool RuleEncodable(const HostRules::RuleKey& k)
    {
        return detail::PluginFieldOk(k.plugin) && detail::IdFieldOk(k.id);
    }

    // The payload for $ML:rules. The message is made safe (SanitizeText). Rows
    // are repeat-free, and an allow row that a deny row also names is left out:
    // deny wins. Rows that cannot travel are counted in stats->dropped; rows
    // past the record or size limit in stats->truncated.
    inline std::string EncodeRules(const RulesSet& s, EncodeStats* stats = nullptr)
    {
        if (stats)
            *stats = EncodeStats{};
        std::string out = kRulesMarker;
        out += kFieldSep;
        out += std::to_string(kRulesVersion);
        out += kFieldSep;
        detail::AppendFlag(out, s.defaultAllow);
        out += kFieldSep;
        out += std::to_string(static_cast<unsigned>(s.onDenied));
        out += kFieldSep;
        out += SanitizeText(s.message, kMaxMessageLen);
        out += kRecordSep;

        const HostRules::DenySet deny = HostRules::Normalize(s.deny);
        const HostRules::DenySet allowAll = HostRules::Normalize(s.allow);
        HostRules::DenySet allow;
        for (const HostRules::RuleKey& k : allowAll)
            if (!HostRules::Contains(deny, k.plugin, k.id))
                allow.push_back(k);

        size_t records = 1;
        bool full = false;
        auto emit = [&](const HostRules::DenySet& rows, bool allowed)
        {
            for (const HostRules::RuleKey& k : rows)
            {
                if (!RuleEncodable(k))
                {
                    if (stats)
                        ++stats->dropped;
                    continue;
                }
                std::string rec = k.plugin;
                rec += kFieldSep;
                rec += k.id;
                rec += kFieldSep;
                detail::AppendFlag(rec, allowed);
                rec += kRecordSep;
                if (full || records >= kMaxRecords || out.size() + rec.size() > kMaxPayloadBytes)
                {
                    full = true;
                    if (stats)
                        ++stats->truncated;
                    continue;
                }
                out += rec;
                ++records;
            }
        };
        emit(deny, false);
        emit(allow, true);
        return out;
    }

    // Reads a $ML:rules payload. Strict, like DecodeFeat; a '$' row is skipped
    // the same way. Deny and allow rows
    // come out repeat-free; an allow row a deny row also names is dropped (deny
    // wins). A version above kRulesVersion is read the way version 1 is, extra
    // fields ignored.
    inline Status DecodeRules(const char* data, size_t len, RulesSet* out)
    {
        std::vector<detail::Fields> recs;
        const Status st = detail::SplitRecords(data, len, &recs);
        if (st != Status::Ok)
            return st;

        const detail::Fields& h = recs[0];
        uint32_t version = 0;
        uint32_t onDenied = 0;
        bool defaultAllow = true;
        if (h.size() < 5 || h[0] != kRulesMarker || !detail::ParseUint(h[1], &version) || version < 1
            || !detail::ParseFlag(h[2], &defaultAllow) || !detail::ParseUint(h[3], &onDenied) || onDenied > 255)
            return Status::BadHeader;
        if (h[4].size() > kMaxMessageLen)
            return Status::FieldTooLong;

        RulesSet s;
        s.defaultAllow = defaultAllow;
        s.onDenied = static_cast<uint8_t>(onDenied);
        s.message.assign(h[4]);

        HostRules::DenySet allowRows;
        for (size_t i = 1; i < recs.size(); ++i)
        {
            const detail::Fields& r = recs[i];
            bool allowed = false;
            if (!r[0].empty() && r[0][0] == '$')
                continue;
            if (r.size() < 3 || r[0].empty() || r[1].empty() || !detail::ParseFlag(r[2], &allowed))
                return Status::BadRecord;
            if (r[0].size() > kMaxPluginLen || r[1].size() > kMaxIdLen)
                return Status::FieldTooLong;

            HostRules::RuleKey k;
            k.plugin.assign(r[0]);
            k.id.assign(r[1]);
            (allowed ? allowRows : s.deny).push_back(std::move(k));
        }
        s.deny = HostRules::Normalize(s.deny);
        for (const HostRules::RuleKey& k : HostRules::Normalize(allowRows))
            if (!HostRules::Contains(s.deny, k.plugin, k.id))
                s.allow.push_back(k);
        *out = std::move(s);
        return Status::Ok;
    }

    // ----------------------------------------------------------- Fragmentation

    // How a payload goes out through the loader's SendWire (network_channel.cpp):
    // it is base64 inside an envelope "[MOD:$ML:<tag>:<size>:]..." and, when the
    // envelope is over one control bunch, cut into MFRG chunks that each carry
    // the label "$ML". kBunchBytes mirrors kMaxBunchPayload in network_channel.cpp;
    // the other limits are the Reassembler's defaults (packet_fragmentation.h).
    constexpr size_t kBunchBytes = 1024;
    constexpr const char* kEnvelopePlugin = "$ML";

    struct SendPlan
    {
        size_t envelopeBytes = 0; // the string SendWire gets
        bool single = true;       // one bunch, no MFRG framing
        size_t chunkBudget = 0;   // payload bytes per chunk when cut, else 0
        size_t chunkCount = 1;
        bool sendable = true;     // false: SendWire would refuse it, or the receiver's limits would
    };

    inline size_t DecimalDigits(size_t n)
    {
        size_t d = 1;
        while (n >= 10)
        {
            n /= 10;
            ++d;
        }
        return d;
    }

    // typeTag is kTagFeat or kTagRules. bunchBytes is the transport's limit.
    inline SendPlan PlanSend(size_t payloadBytes, const char* typeTag, size_t bunchBytes = kBunchBytes)
    {
        SendPlan plan;
        const std::string tag = typeTag ? typeTag : "";
        const std::string label = kEnvelopePlugin;
        const size_t base64 = 4 * ((payloadBytes + 2) / 3);
        // "[MOD:" + plugin + ":" + tag + ":" + size + ":]" + base64
        plan.envelopeBytes = 5 + label.size() + 1 + tag.size() + 1 + DecimalDigits(payloadBytes) + 2 + base64;

        if (plan.envelopeBytes <= bunchBytes)
            return plan;

        plan.single = false;
        if (Fragmentation::kFragHeaderBase + label.size() >= bunchBytes)
        {
            plan.sendable = false;
            plan.chunkCount = 0;
            return plan;
        }
        plan.chunkBudget = bunchBytes - Fragmentation::kFragHeaderBase - label.size();
        plan.chunkCount = (plan.envelopeBytes + plan.chunkBudget - 1) / plan.chunkBudget;
        plan.sendable = plan.envelopeBytes <= Fragmentation::kDefaultMaxMessageBytes
            && plan.chunkCount <= Fragmentation::kDefaultMaxChunkCount;
        return plan;
    }

    // ------------------------------------------------------ Minimum loader tag

    enum class MinCheck
    {
        Ok,           // at or above the minimum
        Below,        // under the minimum
        Unparseable,  // either tag cannot be read: never a reason to act
    };

    namespace detail
    {
        inline bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        // [v]N.N[.N[.N]] then nothing, or '-' or '+' and any printable text.
        // Stricter than AutoUpdateVersion::ParseVersion, which would read
        // "1.2abc" as 1.2.
        inline bool TagWellFormed(const std::string& s)
        {
            if (s.empty() || s.size() > kMaxBuildTagLen)
                return false;
            size_t i = (s[0] == 'v' || s[0] == 'V') ? 1 : 0;
            int parts = 0;
            for (;;)
            {
                const size_t start = i;
                while (i < s.size() && IsDigit(s[i]))
                    ++i;
                if (i == start)
                    return false;
                ++parts;
                if (parts < 4 && i < s.size() && s[i] == '.')
                {
                    ++i;
                    continue;
                }
                break;
            }
            if (parts < 2)
                return false;
            if (i == s.size())
                return true;
            return (s[i] == '-' || s[i] == '+') && !HasCtl(std::string_view(s).substr(i));
        }
    }

    // Is the client's loader build tag under the server's minimum? The tags are
    // the loader's own: "v1.26.1", "1.26.1-dev.7". They compare by number with
    // the loader's AutoUpdateVersion helpers, which ignore a suffix, so a labeled
    // dev build counts as its release part: "1.27.0-dev.3" is not below "1.27.0".
    // A tag that cannot be read, such as the "dev" of an unlabeled build, or an
    // empty one, gives Unparseable and never removes anyone.
    inline MinCheck IsBelowMinimum(const std::string& clientTag, const std::string& minimumTag)
    {
        if (!detail::TagWellFormed(clientTag) || !detail::TagWellFormed(minimumTag))
            return MinCheck::Unparseable;
        const AutoUpdateVersion::ParsedVersion a = AutoUpdateVersion::ParseVersion(clientTag.c_str());
        const AutoUpdateVersion::ParsedVersion b = AutoUpdateVersion::ParseVersion(minimumTag.c_str());
        if (!a.valid || !b.valid)
            return MinCheck::Unparseable;
        return AutoUpdateVersion::CompareVersions(a, b) < 0 ? MinCheck::Below : MinCheck::Ok;
    }
}
