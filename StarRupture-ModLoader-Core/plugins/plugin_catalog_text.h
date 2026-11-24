#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// plugin_catalog_text.h -- the rules for plugin-catalog.v1.txt, the signed
// list of plugins and features the feed publishes next to the version map,
// as plain data.
//
// The signature, the fetch and the cache file belong to the loader; this
// header holds only the grammar of the verified body and the rules for
// replacing the cached copy, with no game, loader or Windows types, so they
// can be tested on their own. Parse takes the body that VersionFeed's sig
// check has already passed (every byte before the `sig` line).
//
// Tab-separated fields, LF-separated rows, UTF-8, no escaping, in this order:
//   format<TAB>1
//   seq<TAB><n>                                 its own sequence, not the version map's
//   kind<TAB>plugin-catalog
//   plugin<TAB><name><TAB><version>             one or more
//   feature<TAB><plugin><TAB><id><TAB><off><TAB><title>   zero or more
// `off` is - (on in every build), * (off in every build) or the game CLs the
// feature is off in: decimal, ascending, comma-separated, no spaces.
// The same row shapes GetDeclaredFeatures serves, so a plugin reads both alike.
//
// A file that breaks any rule is rejected whole. No CR, no empty row, and
// exactly one LF ends the file.
// ---------------------------------------------------------------------------
namespace PluginCatalogText
{
    constexpr size_t kMaxFileBytes    = 256 * 1024;
    constexpr size_t kMaxPlugins      = 200;
    constexpr size_t kMaxFeatures     = 2000;
    constexpr size_t kMaxNameBytes    = 64;
    constexpr size_t kMaxIdBytes      = 64;
    constexpr size_t kMaxVersionBytes = 32;
    constexpr size_t kMaxTitleBytes   = 80;

    struct PluginRow
    {
        std::string name;
        std::string version;
    };

    struct FeatureRow
    {
        std::string plugin;
        std::string id;
        std::string off;    // "-", "*" or "cl,cl,..."
        std::string title;
    };

    struct Catalog
    {
        uint64_t                seq = 0;
        std::vector<PluginRow>  plugins;
        std::vector<FeatureRow> features;
    };

    inline bool IsNameChar(char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    // Letters, digits, '_' and '-', in words joined by single spaces.
    inline bool IsPluginName(std::string_view s)
    {
        if (s.empty() || s.size() > kMaxNameBytes)
            return false;
        bool prevSpace = true;   // a leading space is as wrong as a double one
        for (char c : s)
        {
            if (c == ' ')
            {
                if (prevSpace)
                    return false;
                prevSpace = true;
            }
            else if (IsNameChar(c))
                prevSpace = false;
            else
                return false;
        }
        return !prevSpace;       // no trailing space
    }

    inline bool IsFeatureId(std::string_view s)
    {
        return !s.empty() && s.size() <= kMaxIdBytes && std::all_of(s.begin(), s.end(), IsNameChar);
    }

    inline bool IsVersion(std::string_view s)
    {
        if (s.empty() || s.size() > kMaxVersionBytes)
            return false;
        return std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '+' || c == '-';
        });
    }

    // Well-formed UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
    // Also refuses control characters: C0, DEL and C1 (U+0080..U+009F).
    inline bool IsTitle(std::string_view s)
    {
        if (s.empty() || s.size() > kMaxTitleBytes)
            return false;
        size_t i = 0;
        while (i < s.size())
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
            {
                if (c < 0x20 || c == 0x7F)
                    return false;
                ++i;
                continue;
            }
            size_t len;
            uint32_t cp;
            if (c >= 0xC2 && c <= 0xDF)      { len = 2; cp = c & 0x1F; }
            else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
            else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
            else
                return false;
            if (i + len > s.size())
                return false;
            for (size_t k = 1; k < len; ++k)
            {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80)
                    return false;
                cp = (cp << 6) | (cc & 0x3F);
            }
            if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
                return false;
            if (cp >= 0xD800 && cp <= 0xDFFF)
                return false;
            if (cp >= 0x80 && cp <= 0x9F)
                return false;
            i += len;
        }
        return true;
    }

    // A decimal number with no sign, no leading zero (but "0" itself) and no more than 64 bits.
    inline bool ParseDecimal(std::string_view s, uint64_t& out)
    {
        if (s.empty() || s.size() > 20 || (s.size() > 1 && s[0] == '0'))
            return false;
        uint64_t v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            const uint64_t d = static_cast<uint64_t>(c - '0');
            if (v > (UINT64_MAX - d) / 10)
                return false;
            v = v * 10 + d;
        }
        out = v;
        return true;
    }

    // "-", "*", or CLs of at most 10 digits each, strictly ascending.
    inline bool IsOff(std::string_view s)
    {
        if (s == "-" || s == "*")
            return true;
        uint64_t prev = 0;
        bool first = true;
        size_t pos = 0;
        while (true)
        {
            const size_t comma = s.find(',', pos);
            const std::string_view part = s.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
            uint64_t cl;
            if (part.size() > 10 || !ParseDecimal(part, cl))
                return false;
            if (!first && cl <= prev)
                return false;
            prev = cl;
            first = false;
            if (comma == std::string_view::npos)
                return true;
            pos = comma + 1;
        }
    }

    inline std::string LowerAscii(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    namespace detail
    {
        inline bool Fail(std::string& error, size_t row, const char* why)
        {
            error = "row " + std::to_string(row) + ": " + why;
            return false;
        }

        // Splits on tabs; fewer or more than `want` fields is a failure.
        inline bool Split(std::string_view row, size_t want, std::string_view* out)
        {
            size_t n = 0, pos = 0;
            while (true)
            {
                const size_t tab = row.find('\t', pos);
                if (n == want)
                    return false;
                if (tab == std::string_view::npos)
                {
                    out[n++] = row.substr(pos);
                    return n == want;
                }
                out[n++] = row.substr(pos, tab - pos);
                pos = tab + 1;
            }
        }
    }

    // Reads a verified body. On failure `out` is left empty and `error` says
    // which row broke which rule.
    inline bool Parse(std::string_view body, Catalog& out, std::string& error)
    {
        out = Catalog{};
        error.clear();
        Catalog c;
        bool ok = [&]() {
            if (body.size() > kMaxFileBytes)
                return detail::Fail(error, 0, "file over 256 KiB");
            if (body.empty() || body.back() != '\n')
                return detail::Fail(error, 0, "file does not end with LF");
            if (body.find('\r') != std::string_view::npos)
                return detail::Fail(error, 0, "CR in file");

            // 0 header rows read, then plugins, then features.
            enum class Phase { Format, Seq, Kind, Plugins, Features } phase = Phase::Format;
            std::vector<std::string> pluginKeys;     // lower-cased, for the duplicate check
            std::vector<std::string> featureKeys;    // plugin + TAB + id
            size_t row = 0, pos = 0;
            while (pos < body.size())
            {
                ++row;
                const size_t lf = body.find('\n', pos);
                const std::string_view line = body.substr(pos, lf - pos);
                pos = lf + 1;
                if (line.empty())
                    return detail::Fail(error, row, "empty row");

                std::string_view f[5];
                switch (phase)
                {
                case Phase::Format:
                    if (!detail::Split(line, 2, f) || f[0] != "format" || f[1] != "1")
                        return detail::Fail(error, row, "expected format<TAB>1");
                    phase = Phase::Seq;
                    break;
                case Phase::Seq:
                    if (!detail::Split(line, 2, f) || f[0] != "seq" || !ParseDecimal(f[1], c.seq))
                        return detail::Fail(error, row, "expected seq<TAB><decimal>");
                    phase = Phase::Kind;
                    break;
                case Phase::Kind:
                    if (!detail::Split(line, 2, f) || f[0] != "kind" || f[1] != "plugin-catalog")
                        return detail::Fail(error, row, "expected kind<TAB>plugin-catalog");
                    phase = Phase::Plugins;
                    break;
                default:
                    if (line.compare(0, 7, "plugin\t") == 0)
                    {
                        if (phase != Phase::Plugins)
                            return detail::Fail(error, row, "plugin row after a feature row");
                        if (!detail::Split(line, 3, f))
                            return detail::Fail(error, row, "plugin row needs name and version");
                        if (!IsPluginName(f[1]))
                            return detail::Fail(error, row, "bad plugin name");
                        if (!IsVersion(f[2]))
                            return detail::Fail(error, row, "bad version");
                        if (c.plugins.size() == kMaxPlugins)
                            return detail::Fail(error, row, "more than 200 plugins");
                        std::string key = LowerAscii(f[1]);
                        if (std::find(pluginKeys.begin(), pluginKeys.end(), key) != pluginKeys.end())
                            return detail::Fail(error, row, "duplicate plugin");
                        pluginKeys.push_back(std::move(key));
                        c.plugins.push_back({ std::string(f[1]), std::string(f[2]) });
                    }
                    else if (line.compare(0, 8, "feature\t") == 0)
                    {
                        if (c.plugins.empty())
                            return detail::Fail(error, row, "feature row before any plugin row");
                        phase = Phase::Features;
                        if (!detail::Split(line, 5, f))
                            return detail::Fail(error, row, "feature row needs plugin, id, off and title");
                        if (std::none_of(c.plugins.begin(), c.plugins.end(), [&](const PluginRow& p) { return p.name == f[1]; }))
                            return detail::Fail(error, row, "feature names an unknown plugin");
                        if (!IsFeatureId(f[2]))
                            return detail::Fail(error, row, "bad feature id");
                        if (!IsOff(f[3]))
                            return detail::Fail(error, row, "bad off field");
                        if (!IsTitle(f[4]))
                            return detail::Fail(error, row, "bad title");
                        if (c.features.size() == kMaxFeatures)
                            return detail::Fail(error, row, "more than 2000 features");
                        std::string key = std::string(f[1]) + '\t' + std::string(f[2]);
                        if (std::find(featureKeys.begin(), featureKeys.end(), key) != featureKeys.end())
                            return detail::Fail(error, row, "duplicate feature");
                        featureKeys.push_back(std::move(key));
                        c.features.push_back({ std::string(f[1]), std::string(f[2]), std::string(f[3]), std::string(f[4]) });
                    }
                    else
                        return detail::Fail(error, row, "unknown row");
                    break;
                }
            }
            if (c.plugins.empty())
                return detail::Fail(error, row + 1, "no plugin row");
            return true;
        }();
        if (ok)
            out = std::move(c);
        return ok;
    }

    // The text GetFeatureCatalog serves: the parsed catalog in the file's own
    // grammar. For a canonical input this is the input, byte for byte.
    inline std::string ToAccessorText(const Catalog& c)
    {
        std::string out = "format\t1\nseq\t" + std::to_string(c.seq) + "\nkind\tplugin-catalog\n";
        for (const PluginRow& p : c.plugins)
            out += "plugin\t" + p.name + '\t' + p.version + '\n';
        for (const FeatureRow& f : c.features)
            out += "feature\t" + f.plugin + '\t' + f.id + '\t' + f.off + '\t' + f.title + '\n';
        return out;
    }

    // The last good copy: its seq and the verified body it came from.
    struct Cached
    {
        uint64_t    seq = 0;
        std::string body;
    };

    enum class CacheDecision
    {
        Replace,           // no cache, or a higher seq: take the candidate
        Unchanged,         // same seq, same bytes: nothing to do
        RefuseRollback,    // lower seq than the cache
        RefuseConflict,    // same seq, different bytes
    };

    // What to do with a candidate that parsed. `cached` is null when there is no
    // cache. Anything but Replace keeps the cache as it is; so does a candidate
    // that failed to verify or parse, which never gets here.
    inline CacheDecision DecideCache(const Cached* cached, uint64_t candidateSeq, std::string_view candidateBody)
    {
        if (!cached || candidateSeq > cached->seq)
            return CacheDecision::Replace;
        if (candidateSeq < cached->seq)
            return CacheDecision::RefuseRollback;
        return cached->body == candidateBody ? CacheDecision::Unchanged : CacheDecision::RefuseConflict;
    }
}
