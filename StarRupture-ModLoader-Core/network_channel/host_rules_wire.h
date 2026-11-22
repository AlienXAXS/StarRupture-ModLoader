#pragma once

// What a server and a client say to each other about host rules, as bytes.
//
// Two loader-internal messages ride the "$ML" envelope (the same way the
// manifest and the wire table do):
//
//   $ML:feat   client -> server. Who the client is and which plugin features it
//              declares, with whether each is on for this game build.
//   $ML:rules  server -> client. The server's rule set: a default, a note the
//              client writes to its ModLoader.log and one row per feature or
//              mod it names.
//
// Both payloads are records ending in kRecordSep, fields split on kFieldSep,
// the same separators as the wire table (wire_compat.h), so a plugin name or a
// title can hold any printable text. A field never holds a control character.
//
//   $ML:feat payload
//     $loader US <build tag> US <interface version> RS          (first record)
//     $game US <CL> US <image stamp, hex> US <image size, hex> RS  (optional)
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
#include <cstdio>
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
    constexpr const char* kGameMarker = "$game";
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

        // Hexadecimal digits only, no prefix, at most 8.
        inline bool ParseHex(std::string_view s, uint32_t* out)
        {
            if (s.empty() || s.size() > 8)
                return false;
            uint32_t v = 0;
            for (char c : s)
            {
                uint32_t d;
                if (c >= '0' && c <= '9')
                    d = static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    d = static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    d = static_cast<uint32_t>(c - 'A' + 10);
                else
                    return false;
                v = (v << 4) | d;
            }
            *out = v;
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
        uint32_t gameCL = 0;           // the game build the client runs; 0 = not sent
        uint32_t gameImageStamp = 0;
        uint32_t gameImageSize = 0;
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
        if (p.gameCL != 0)
        {
            // Right after the header, so a long feature list never cuts it.
            char hex[24];
            std::snprintf(hex, sizeof(hex), "%x%c%x", static_cast<unsigned>(p.gameImageStamp), kFieldSep,
                          static_cast<unsigned>(p.gameImageSize));
            out += kGameMarker;
            out += kFieldSep;
            out += std::to_string(p.gameCL);
            out += kFieldSep;
            out += hex;
            out += kRecordSep;
            ++records;
        }
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
    // with '$' is skipped as a record kind this reader does not know. The one
    // such record read here is "$game", leniently: a "$game" row that is not
    // well formed is ignored and the rest still decodes.
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
            {
                uint32_t cl = 0, stamp = 0, size = 0;
                if (r[0] == kGameMarker && r.size() >= 4 && detail::ParseUint(r[1], &cl) && cl != 0 &&
                    detail::ParseHex(r[2], &stamp) && detail::ParseHex(r[3], &size))
                {
                    p.gameCL = cl;
                    p.gameImageStamp = stamp;
                    p.gameImageSize = size;
                }
                continue;
            }
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
    // default or a wider row denies it. Which row wins for a given
    // feature (feature row, then whole-mod row, then default) is ResolveDeny's.
    struct RulesSet
    {
        bool defaultAllow = true;
        uint8_t onDenied = kOnDeniedSwitchOff;
        std::string message; // plain text the client writes to its ModLoader.log, at most kMaxMessageLen bytes
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
    // wins). A version above kRulesVersion is read the way version 1 is: the
    // header and rows are taken as they are, but a record with more than
    // kMaxFieldsPerRecord fields is refused (BadRecord), as in version 1.
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

    // What the client switches off: for each feature the client declares, the
    // server's answer under the precedence of the design (section 7, 11.2):
    //   1. a row naming the feature itself ("Plugin" + id), deny or allow;
    //   2. else a row naming the whole mod ("Plugin" + "*"), deny or allow;
    //   3. else the default (deny when !defaultAllow).
    // The most specific row wins, so an allow row beats a wider deny and a deny
    // row beats a wider allow. Two rows of the same level that disagree
    // (possible only in a RulesSet that did not come through DecodeRules): deny
    // wins, the safer reading and the one encode and decode apply too. The
    // result is the exact (plugin, id) list ApplyHostRules takes, in the order
    // declared, spelled as the client declared it; plugin names match without
    // regard to case, ids exactly. A feature already off for the build is
    // listed like any other: the server can only take a feature away, so the
    // list never turns anything on. Rules for a mod or feature the client does
    // not declare name nothing here; the caller resolves again when its
    // catalog changes.
    inline HostRules::DenySet ResolveDeny(const RulesSet& rules, const FeatPayload& declared)
    {
        HostRules::DenySet out;
        for (const FeatRecord& f : declared.features)
        {
            if (f.plugin.empty() || f.id.empty())
                continue;
            bool deny = !rules.defaultAllow;
            if (HostRules::Contains(rules.deny, f.plugin, f.id))
                deny = true;
            else if (HostRules::Contains(rules.allow, f.plugin, f.id))
                deny = false;
            else if (HostRules::Contains(rules.deny, f.plugin, kWholePlugin))
                deny = true;
            else if (HostRules::Contains(rules.allow, f.plugin, kWholePlugin))
                deny = false;
            if (deny)
                out.push_back(HostRules::RuleKey{ f.plugin, f.id });
        }
        return HostRules::Normalize(out);
    }

    // ---------------------------------------------------------- Rules as text

    // How the server's operator (or a server plugin) writes a rule set. One
    // line each; LF or CRLF; a UTF-8 BOM at the start is ignored:
    //
    //   # a comment (also a line starting with ';'); blank lines are skipped
    //   default=allow|deny           what a feature no row names gets (once)
    //   message=<text>               the note the client writes to its ModLoader.log, up to kMaxMessageLen bytes (once)
    //   Plugin.feature=allow|deny    one feature of a mod
    //   Plugin.*=allow|deny          the whole mod
    //
    // Keys and values ignore case, except the feature id, which is exact. The
    // key splits at its first '.', so an id may hold dots. Spaces and tabs
    // around a line, a key and a value are trimmed; a message keeps the rest of
    // its line as written, so it may hold '#' and '='. A key naming the same
    // feature twice is not an error: deny wins, as on the wire. Text with no
    // rule lines is an empty set that allows everything.
    enum class ParseStatus
    {
        Ok,
        TooLarge,       // over kMaxPayloadBytes
        BadLine,        // not a comment, and no '='
        BadKey,         // not default, message or Plugin.feature
        BadValue,       // not allow or deny
        Duplicate,      // default or message given twice
        MessageTooLong, // message over kMaxMessageLen bytes
        ControlChar,    // a control character inside a line
        TooManyRows,    // more rows than one $ML:rules payload can hold
    };

    struct ParseResult
    {
        ParseStatus status = ParseStatus::Ok;
        size_t line = 0; // 1-based; 0 for TooLarge
    };

    namespace detail
    {
        inline std::string_view TrimBlanks(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            return s;
        }
    }

    // Reads rules text into *out. On any error `out` is left as it was and
    // `error` holds a line for the log or the console, "line N: ...". On Ok
    // `error` is empty and `out` is the whole set, with the on-denied mode
    // kOnDeniedSwitchOff.
    inline ParseResult ParseRulesText(std::string_view text, RulesSet& out, std::string& error)
    {
        ParseResult res;
        error.clear();
        const auto fail = [&](ParseStatus st, size_t line, const std::string& why)
        {
            res.status = st;
            res.line = line;
            error = line ? "line " + std::to_string(line) + ": " + why : why;
            return res;
        };

        if (text.size() > kMaxPayloadBytes)
            return fail(ParseStatus::TooLarge, 0, "text is over " + std::to_string(kMaxPayloadBytes) + " bytes");
        if (text.substr(0, 3) == "\xEF\xBB\xBF")
            text.remove_prefix(3);

        RulesSet s;
        bool haveDefault = false;
        bool haveMessage = false;
        size_t lineNo = 0;
        while (!text.empty())
        {
            ++lineNo;
            const size_t nl = text.find('\n');
            std::string_view line = text.substr(0, nl);
            text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            line = detail::TrimBlanks(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;

            // Spaces and tabs around the key and the value are blanks; a control
            // character anywhere else is an error.
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos)
                return detail::HasCtl(line) ? fail(ParseStatus::ControlChar, lineNo, "control character")
                                            : fail(ParseStatus::BadLine, lineNo, "expected key=value");
            const std::string key(detail::TrimBlanks(line.substr(0, eq)));
            const std::string value(detail::TrimBlanks(line.substr(eq + 1)));
            if (detail::HasCtl(key))
                return fail(ParseStatus::ControlChar, lineNo, "control character");
            if (detail::HasCtl(value))
                return fail(ParseStatus::ControlChar, lineNo, "control character");

            if (HostRules::SamePlugin(key, "message"))
            {
                if (haveMessage)
                    return fail(ParseStatus::Duplicate, lineNo, "message is given twice");
                if (value.size() > kMaxMessageLen)
                    return fail(ParseStatus::MessageTooLong, lineNo, "message is over " + std::to_string(kMaxMessageLen) + " bytes");
                haveMessage = true;
                s.message = value;
                continue;
            }

            const bool isDefault = HostRules::SamePlugin(key, "default");
            HostRules::RuleKey rk;
            if (!isDefault)
            {
                const size_t dot = key.find('.');
                if (dot != std::string::npos)
                {
                    rk.plugin = key.substr(0, dot);
                    rk.id = key.substr(dot + 1);
                }
                if (dot == std::string::npos || !RuleEncodable(rk))
                    return fail(ParseStatus::BadKey, lineNo, "key must be default, message, Plugin.feature or Plugin.*");
            }
            bool allowed = false;
            if (HostRules::SamePlugin(value, "allow"))
                allowed = true;
            else if (!HostRules::SamePlugin(value, "deny"))
                return fail(ParseStatus::BadValue, lineNo, "value must be allow or deny");

            if (isDefault)
            {
                if (haveDefault)
                    return fail(ParseStatus::Duplicate, lineNo, "default is given twice");
                haveDefault = true;
                s.defaultAllow = allowed;
                continue;
            }

            // A feature named twice: deny wins, as EncodeRules and DecodeRules read it.
            if (allowed)
            {
                if (HostRules::Contains(s.deny, rk.plugin, rk.id) || HostRules::Contains(s.allow, rk.plugin, rk.id))
                    continue;
            }
            else
            {
                for (auto it = s.allow.begin(); it != s.allow.end(); ++it)
                    if (HostRules::SameRule(*it, rk))
                    {
                        s.allow.erase(it);
                        break;
                    }
                if (HostRules::Contains(s.deny, rk.plugin, rk.id))
                    continue;
            }
            if (s.deny.size() + s.allow.size() + 1 >= kMaxRecords) // the header record counts
                return fail(ParseStatus::TooManyRows, lineNo, "more than " + std::to_string(kMaxRecords - 1) + " rules");
            (allowed ? s.allow : s.deny).push_back(std::move(rk));
        }
        out = std::move(s);
        return res;
    }

    // ------------------------------------------------------ Console and per player

    // The rules a connection with no set of its own is given: the global set
    // with the host console's list (features hostoff) added to its deny rows,
    // and the console's note (features hostmsg) in place of the global one when
    // it says anything. The default and the on-denied mode stay the global's.
    inline RulesSet MergeConsole(const RulesSet& global, const HostRules::DenySet& consoleDeny, const std::string& consoleMessage)
    {
        RulesSet m = global;
        m.deny.insert(m.deny.end(), consoleDeny.begin(), consoleDeny.end());
        m.deny = HostRules::Normalize(m.deny);
        const std::string note = SanitizeText(consoleMessage, kMaxMessageLen);
        if (!note.empty())
            m.message = note;
        return m;
    }

    // What a connection is sent: its own set when it has one (perConnection not
    // null), taken as it is, so the console list and the console note do not
    // apply to it; otherwise the global set merged with the console's.
    inline RulesSet EffectiveRules(const RulesSet& global, const RulesSet* perConnection,
        const HostRules::DenySet& consoleDeny, const std::string& consoleMessage)
    {
        return perConnection ? *perConnection : MergeConsole(global, consoleDeny, consoleMessage);
    }

    // True when a set says nothing to a client: it allows everything and has no
    // note a player could read. A client that was never sent a set needs none.
    inline bool NothingToSend(const RulesSet& s)
    {
        return s.defaultAllow && s.deny.empty() && SanitizeText(s.message, kMaxMessageLen).empty();
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

    // ------------------------------------------------ Minimum versions, per mod

    // The host's version table: the least ModLoader a client may run and the
    // least version of each mod. Written as text, read by ParseMinimumsText:
    //
    //   # a comment (also a line starting with ';'); blank lines are skipped
    //   loader=<version>        the minimum ModLoader; empty or absent = off
    //   Plugin=<version>        the minimum version of one mod
    //   Plugin=wire             the player's copy of the mod must be one the wire
    //                           check accepts (wire_compat.h); no number
    //
    // Same line rules as the rules text: LF or CRLF, a UTF-8 BOM at the start
    // ignored, spaces and tabs trimmed around a line, a key and a value, a
    // control character inside a line is an error. Keys ignore case. A version
    // is what IsBelowMinimum can read: "0.11.0", "v0.11.0", "1.27.0-dev.3". One
    // it cannot read is refused, never stored. A key given twice is an error.
    // The key "loader" is the loader's; a mod cannot be named that.
    struct ModMinimum
    {
        std::string plugin;
        std::string version; // empty when wire
        bool wire = false;
    };

    struct Minimums
    {
        std::string loader; // empty = no loader minimum
        std::vector<ModMinimum> mods;

        bool Empty() const { return loader.empty() && mods.empty(); }
    };

    // Most rows one table may hold.
    constexpr size_t kMaxModMinimums = 512;

    // Reads minimums text into *out. On any error `out` is left as it was and
    // `error` holds a line for the log or the console, "line N: ...". On Ok
    // `error` is empty and `out` is the whole table. Empty or comment-only
    // text is an empty table (everything off). Statuses: TooLarge, BadLine (no
    // '='), BadKey (a mod name that cannot be sent), BadValue (a version that
    // cannot be read, or a mod row with no value), Duplicate, ControlChar,
    // TooManyRows.
    inline ParseResult ParseMinimumsText(std::string_view text, Minimums& out, std::string& error)
    {
        ParseResult res;
        error.clear();
        const auto fail = [&](ParseStatus st, size_t line, const std::string& why)
        {
            res.status = st;
            res.line = line;
            error = line ? "line " + std::to_string(line) + ": " + why : why;
            return res;
        };

        if (text.size() > kMaxPayloadBytes)
            return fail(ParseStatus::TooLarge, 0, "text is over " + std::to_string(kMaxPayloadBytes) + " bytes");
        if (text.substr(0, 3) == "\xEF\xBB\xBF")
            text.remove_prefix(3);

        Minimums m;
        bool haveLoader = false;
        size_t lineNo = 0;
        while (!text.empty())
        {
            ++lineNo;
            const size_t nl = text.find('\n');
            std::string_view line = text.substr(0, nl);
            text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            line = detail::TrimBlanks(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;

            // Spaces and tabs around the key and the value are blanks; a control
            // character anywhere else is an error.
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos)
                return detail::HasCtl(line) ? fail(ParseStatus::ControlChar, lineNo, "control character")
                                            : fail(ParseStatus::BadLine, lineNo, "expected key=value");
            const std::string key(detail::TrimBlanks(line.substr(0, eq)));
            const std::string value(detail::TrimBlanks(line.substr(eq + 1)));
            if (detail::HasCtl(key))
                return fail(ParseStatus::ControlChar, lineNo, "control character");
            if (detail::HasCtl(value))
                return fail(ParseStatus::ControlChar, lineNo, "control character");

            if (HostRules::SamePlugin(key, "loader"))
            {
                if (haveLoader)
                    return fail(ParseStatus::Duplicate, lineNo, "loader is given twice");
                haveLoader = true;
                if (!value.empty() && !detail::TagWellFormed(value))
                    return fail(ParseStatus::BadValue, lineNo, "loader needs a version such as 1.27.0, or nothing to turn it off");
                m.loader = value;
                continue;
            }

            if (!detail::PluginFieldOk(key))
                return fail(ParseStatus::BadKey, lineNo, "key must be loader or a mod name");
            for (const ModMinimum& have : m.mods)
                if (HostRules::SamePlugin(have.plugin, key))
                    return fail(ParseStatus::Duplicate, lineNo, key + " is given twice");

            ModMinimum row;
            row.plugin = key;
            if (HostRules::SamePlugin(value, "wire"))
                row.wire = true;
            else if (detail::TagWellFormed(value))
                row.version = value;
            else
                return fail(ParseStatus::BadValue, lineNo, key + " needs a version such as 0.11.0, or wire");
            if (m.mods.size() >= kMaxModMinimums)
                return fail(ParseStatus::TooManyRows, lineNo, "more than " + std::to_string(kMaxModMinimums) + " mods");
            m.mods.push_back(std::move(row));
        }
        out = std::move(m);
        return res;
    }

    // One mod of the client's manifest, as FindBelowMinimum needs it.
    // wireRefused is the caller's answer for this mod on this connection:
    // ClassifyPair gave PairState::Incompatible. It is read only for a
    // Plugin=wire row.
    struct ManifestRow
    {
        std::string plugin;
        std::string version;
        bool wireRefused = false;
    };

    // One thing a client is below. need is the number the host asked for, empty
    // for a Plugin=wire row. have is what the client runs, empty when it has no
    // version to show (a loader too old to report one).
    struct Violation
    {
        bool isLoader = false;
        std::string name; // "ModLoader", or the mod's name as the client reported it
        std::string need;
        std::string have;
    };

    // What the client is below, loader first, then mods in manifest order.
    //   loaderReported  the client sent its loader tag ($ML:feat); false is a
    //                   loader older than anything with the check
    //   loaderTag       that tag ("" when not reported)
    // A loader that did not report is below any loader minimum. A tag that
    // cannot be read ("dev", empty) never is. A mod the host named and the
    // client does not have, a mod version that cannot be read, and a mod with
    // no row are never below. A suffix is ignored: "0.11.0-dev.6" is "0.11.0".
    inline std::vector<Violation> FindBelowMinimum(const Minimums& table, bool loaderReported, const std::string& loaderTag,
                                                   const std::vector<ManifestRow>& manifest)
    {
        std::vector<Violation> out;
        if (!table.loader.empty() && detail::TagWellFormed(table.loader))
        {
            const bool below = loaderReported ? IsBelowMinimum(loaderTag, table.loader) == MinCheck::Below : true;
            if (below)
                out.push_back({ true, "ModLoader", table.loader, loaderReported ? loaderTag : std::string() });
        }
        for (const ManifestRow& row : manifest)
        {
            for (const ModMinimum& want : table.mods)
            {
                if (!HostRules::SamePlugin(want.plugin, row.plugin))
                    continue;
                const bool below = want.wire ? row.wireRefused : IsBelowMinimum(row.version, want.version) == MinCheck::Below;
                if (below)
                    out.push_back({ false, row.plugin, want.version, row.version });
                break;
            }
        }
        return out;
    }

    // The most characters a below-minimum message has. Mod names are cut at the
    // end to stay under it.
    constexpr size_t kMaxBelowMinimumReason = 240;

    namespace detail
    {
        // A name for the message: plain ASCII, no control characters, cut to maxLen.
        inline std::string PlainName(const std::string& in, size_t maxLen)
        {
            std::string s;
            for (char c : in)
            {
                if (s.size() >= maxLen)
                    break;
                s += (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) ? '?' : c;
            }
            return s;
        }

        // Version tags are cut to this many characters in the message, so the
        // closing instruction survives the final cut to kMaxBelowMinimumReason.
        constexpr size_t kMaxShownTagLen = 24;

        // "RuptureTimer 0.11.0 or newer (you have 0.9.2)", "ModLoader 1.27.0 or
        // newer", "ProductionViewer to match the server's version (you have 0.18.4)".
        inline std::string NeedPhrase(const Violation& v, size_t nameLen)
        {
            std::string s = v.isLoader ? v.name : PlainName(v.name, nameLen);
            s += v.need.empty() ? " to match the server's version" : " " + PlainName(v.need, kMaxShownTagLen) + " or newer";
            if (!v.have.empty())
                s += " (you have " + PlainName(v.have, kMaxShownTagLen) + ")";
            return s;
        }

        inline std::string BelowMinimumText(const std::vector<Violation>& v, size_t nameLen)
        {
            const Violation& first = v.front();
            std::string s = "This server needs " + NeedPhrase(first, nameLen);
            if (v.size() == 1)
            {
                if (first.isLoader)
                    return s + ". Update it, or remove the ModLoader to join without mods.";
                const std::string name = PlainName(first.name, nameLen);
                return s + ". Update " + name + ", or remove it, and join again.";
            }
            s += " and " + NeedPhrase(v[1], nameLen);
            if (v.size() > 2)
                s += " and " + std::to_string(v.size() - 2) + " more";
            return s + ". Update them and join again.";
        }
    }

    // The plain text a player sees when FindBelowMinimum found something: one
    // line, ASCII, at most kMaxBelowMinimumReason characters ("" when there is
    // nothing). The loader line and the mod lines are worded for the player; the
    // text never mentions the version object map.
    inline std::string BuildBelowMinimumReason(const std::vector<Violation>& violations)
    {
        if (violations.empty())
            return {};
        size_t nameLen = kMaxPluginLen;
        std::string s = detail::BelowMinimumText(violations, nameLen);
        while (s.size() > kMaxBelowMinimumReason && nameLen > 1)
        {
            nameLen = nameLen > 16 ? nameLen / 2 : nameLen - 1;
            s = detail::BelowMinimumText(violations, nameLen);
        }
        if (s.size() > kMaxBelowMinimumReason)
            s.resize(kMaxBelowMinimumReason);
        return s;
    }

    // What one client's loader reported, as LF-separated text for a server
    // plugin: "loader=<build tag> interface=<version>", then one
    // "<Plugin>.<id>=on|off" line per feature, in the order reported.
    inline std::string BuildClientFeaturesText(const std::string& loaderTag, uint32_t interfaceVersion,
                                               const std::vector<FeatRecord>& features)
    {
        std::string s = "loader=" + loaderTag + " interface=" + std::to_string(interfaceVersion);
        for (const FeatRecord& f : features)
        {
            s += '\n';
            s += f.plugin;
            s += '.';
            s += f.id;
            s += f.on ? "=on" : "=off";
        }
        return s;
    }

    // snprintf-style copy for a caller's buffer: returns the full length of
    // `text`, writes it NUL-terminated and cut to `size`; with a null buf or a
    // size of 0 nothing is written (a measure-only call).
    inline int CopyTextCut(const std::string& text, char* buf, size_t size)
    {
        if (buf && size > 0)
        {
            const size_t n = text.size() < size - 1 ? text.size() : size - 1;
            text.copy(buf, n);
            buf[n] = '\0';
        }
        return static_cast<int>(text.size());
    }

    // ------------------------------------------------- $ML:adminq, $ML:adminr

    // The admin prompt. A server plugin asks a joining player for an admin code
    // ($ML:adminq, server to client); the client answers with the code or with
    // "not me" ($ML:adminr, client to server). Both are one record:
    // "<kind> US <text> RS".
    constexpr const char* kTagAdminQ = "adminq";
    constexpr const char* kTagAdminR = "adminr";

    constexpr const char* kAdminKindAsk = "ask";     // open the prompt
    constexpr const char* kAdminKindInfo = "info";   // a line for the open prompt, it stays open
    constexpr const char* kAdminKindClose = "close"; // a last line, then the prompt closes
    constexpr const char* kAdminKindCode = "code";   // the player typed a code
    constexpr const char* kAdminKindNotMe = "notme"; // the player says they are not an admin

    constexpr size_t kMaxAdminQText = kMaxMessageLen; // a line the server shows
    constexpr size_t kMaxAdminRCode = 64;             // a code the player typed
    // Longest valid payloads ("close" + US + text + RS, "notme" + US + RS). A
    // longer payload is refused before it is cut into records.
    constexpr size_t kMaxAdminQPayload = 5 + 1 + kMaxAdminQText + 1;
    constexpr size_t kMaxAdminRPayload = 5 + 1 + kMaxAdminRCode + 1;

    // The least time between two accepted answers from one connection.
    constexpr uint64_t kAdminAnswerMinGapMs = 500;

    enum class AdminQKind { Ask, Info, Close };
    enum class AdminRKind { Code, NotMe };

    struct AdminQuestion
    {
        AdminQKind kind = AdminQKind::Ask;
        std::string text; // at most kMaxAdminQText bytes, may be empty
    };

    struct AdminAnswer
    {
        AdminRKind kind = AdminRKind::Code;
        std::string code; // 1 to kMaxAdminRCode bytes for Code, empty for NotMe
    };

    namespace detail
    {
        inline const char* AdminQKindName(AdminQKind k)
        {
            switch (k)
            {
            case AdminQKind::Ask: return kAdminKindAsk;
            case AdminQKind::Info: return kAdminKindInfo;
            case AdminQKind::Close: return kAdminKindClose;
            }
            return kAdminKindAsk;
        }

        // The shape shared by both: one record of exactly two fields, within
        // maxPayload bytes. Status::Ok leaves the fields in *kind and *text.
        inline Status SplitAdmin(const char* data, size_t len, size_t maxPayload,
                                 std::string_view* kind, std::string_view* text)
        {
            if (!data || len == 0)
                return Status::Empty;
            if (len > maxPayload)
                return Status::TooLarge;
            std::vector<Fields> recs;
            const Status st = SplitRecords(data, len, &recs);
            if (st != Status::Ok)
                return st;
            if (recs.size() != 1 || recs[0].size() != 2)
                return Status::BadRecord;
            *kind = recs[0][0];
            *text = recs[0][1];
            return Status::Ok;
        }
    }

    // The payload for $ML:adminq. The text is made safe (controls to spaces,
    // trimmed, cut to kMaxAdminQText bytes on a UTF-8 boundary).
    inline std::string EncodeAdminQuestion(AdminQKind kind, const std::string& text)
    {
        std::string out = detail::AdminQKindName(kind);
        out += kFieldSep;
        out += SanitizeText(text, kMaxAdminQText);
        out += kRecordSep;
        return out;
    }

    // Reads a $ML:adminq payload. Strict: a wrong shape, an unknown kind
    // (BadRecord), text over kMaxAdminQText (FieldTooLong) or a control
    // character (ControlChar) is an error and *out stays as it was.
    inline Status DecodeAdminQuestion(const char* data, size_t len, AdminQuestion* out)
    {
        std::string_view kind, text;
        const Status st = detail::SplitAdmin(data, len, kMaxAdminQPayload, &kind, &text);
        if (st != Status::Ok)
            return st;
        AdminQuestion q;
        if (kind == kAdminKindAsk)
            q.kind = AdminQKind::Ask;
        else if (kind == kAdminKindInfo)
            q.kind = AdminQKind::Info;
        else if (kind == kAdminKindClose)
            q.kind = AdminQKind::Close;
        else
            return Status::BadRecord;
        if (text.size() > kMaxAdminQText)
            return Status::FieldTooLong;
        q.text.assign(text);
        *out = std::move(q);
        return Status::Ok;
    }

    // The payload for $ML:adminr. A code is made safe like a message and cut to
    // kMaxAdminRCode bytes; a code that is empty after that cannot travel and
    // gives "" (send nothing). NotMe carries no code.
    inline std::string EncodeAdminAnswer(AdminRKind kind, const std::string& code)
    {
        std::string out;
        if (kind == AdminRKind::Code)
        {
            const std::string safe = SanitizeText(code, kMaxAdminRCode);
            if (safe.empty())
                return out;
            out = kAdminKindCode;
            out += kFieldSep;
            out += safe;
        }
        else
        {
            out = kAdminKindNotMe;
            out += kFieldSep;
        }
        out += kRecordSep;
        return out;
    }

    // Reads a $ML:adminr payload. Strict like DecodeAdminQuestion; also a code
    // answer with no code, or a not-me answer with one, is BadRecord.
    inline Status DecodeAdminAnswer(const char* data, size_t len, AdminAnswer* out)
    {
        std::string_view kind, text;
        const Status st = detail::SplitAdmin(data, len, kMaxAdminRPayload, &kind, &text);
        if (st != Status::Ok)
            return st;
        AdminAnswer a;
        if (kind == kAdminKindCode)
        {
            a.kind = AdminRKind::Code;
            if (text.empty())
                return Status::BadRecord;
            if (text.size() > kMaxAdminRCode)
                return Status::FieldTooLong;
            a.code.assign(text);
        }
        else if (kind == kAdminKindNotMe)
        {
            a.kind = AdminRKind::NotMe;
            if (!text.empty())
                return Status::BadRecord;
        }
        else
            return Status::BadRecord;
        *out = std::move(a);
        return Status::Ok;
    }

    // Whether the server takes an admin answer from one connection.
    enum class AdminAnswerDecision
    {
        Accept,      // the connection was asked and is not over the rate limit
        NotAsked,    // no prompt is open for it: ignore the answer
        RateLimited, // under minGapMs since its last accepted answer: drop it
    };

    // `asked`: a prompt is open for this connection. `hasLast` and
    // `lastAnswerMs`: the time of its last accepted answer, if any. The caller
    // records nowMs as the new last answer only on Accept. A clock that reads
    // earlier than lastAnswerMs counts as inside the gap.
    inline AdminAnswerDecision DecideAdminAnswer(bool asked, uint64_t nowMs, bool hasLast, uint64_t lastAnswerMs,
                                                 uint64_t minGapMs = kAdminAnswerMinGapMs)
    {
        if (!asked)
            return AdminAnswerDecision::NotAsked;
        if (hasLast && (nowMs < lastAnswerMs || nowMs - lastAnswerMs < minGapMs))
            return AdminAnswerDecision::RateLimited;
        return AdminAnswerDecision::Accept;
    }

    // A gap between two ticks over this is an engine stall (a level load, a
    // hitch), not time the client had to report its loader version in.
    constexpr uint64_t kStallGapMs = 2000;

    // How much of a tick gap to add to the loader-report grace: the whole gap
    // when it is a stall, nothing otherwise.
    inline uint64_t StallExtensionMs(uint64_t gapMs)
    {
        return gapMs > kStallGapMs ? gapMs : 0;
    }

    // True once a connection that sent a manifest has had graceMs, plus the
    // stalled time, to send its feature list. A clock that reads earlier than
    // the manifest is not over.
    inline bool LoaderGraceOver(uint64_t nowMs, uint64_t manifestMs, uint64_t extraMs, uint64_t graceMs)
    {
        return nowMs >= manifestMs && nowMs - manifestMs >= graceMs + extraMs;
    }

    // At most one plugin announce and rules re-push per connection per this long
    // when its "$ML:feat" messages come in a burst.
    constexpr uint64_t kFeatActGapMs = 1000;

    enum class FeatGate
    {
        Act,   // announce the plugins and re-push the rules now
        Defer, // inside the gap: keep the data, act after the gap
    };

    // `hasPrev` and `lastActMs`: when this connection last acted on a feature
    // list, if it did. A clock that reads earlier than lastActMs defers.
    inline FeatGate DecideFeatAct(bool hasPrev, uint64_t nowMs, uint64_t lastActMs, uint64_t gapMs = kFeatActGapMs)
    {
        if (!hasPrev)
            return FeatGate::Act;
        return nowMs >= lastActMs && nowMs - lastActMs >= gapMs ? FeatGate::Act : FeatGate::Defer;
    }

    // True when a client and a server run different game builds: both builds
    // known (non-zero) and not equal. A build that is not known, as from an
    // older loader, is never a mismatch.
    inline bool GameBuildMismatch(uint32_t clientCL, uint32_t serverCL)
    {
        return clientCL != 0 && serverCL != 0 && clientCL != serverCL;
    }
}
