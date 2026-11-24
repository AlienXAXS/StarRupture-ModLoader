#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// ---------------------------------------------------------------------------
// declared_features_text.h -- the text IPluginRegistry::GetDeclaredFeatures
// hands out and the loader writes to ModLoader\Logs\declared-features.v1.txt,
// as plain data.
//
// The plugin table and the feature catalog belong to plugin_manager.cpp and
// plugin_hook_report.cpp; this header holds only the text rules, with no game,
// loader or Windows types, so they can be tested on their own.
//
// Tab-separated fields, LF-separated rows, UTF-8, no escaping:
//   format<TAB>1
//   kind<TAB>declared-features
//   game_cl<TAB><running game CL>
//   loader<TAB><loader build tag>
//   plugin<TAB><name><TAB><version>                          one per running plugin
//   feature<TAB><plugin><TAB><id><TAB><state><TAB><title>    state is on or off
// Sorted by plugin, then id, without regard to case.
// ---------------------------------------------------------------------------
namespace DeclaredFeaturesText
{
    // The longest title kept, in bytes.
    constexpr size_t kMaxTitleBytes = 80;

    // A plugin that is running (same rule as GetPluginVersion).
    struct PluginRow
    {
        std::string name;     // PluginInfo name
        std::string version;  // PluginInfo version
    };

    // One declared feature. `on` is the loader's verdict for this game build.
    struct FeatureRow
    {
        std::string plugin;
        std::string id;
        std::string title;
        bool        on = true;
    };

    // A tab, CR or LF would break the row format: each becomes a space.
    inline std::string SanitiseField(const std::string& in)
    {
        std::string out = in;
        for (char& c : out)
            if (c == '\t' || c == '\r' || c == '\n')
                c = ' ';
        return out;
    }

    // As SanitiseField, and every other control character too; then cut to
    // kMaxTitleBytes without splitting a UTF-8 character.
    inline std::string SanitiseTitle(const std::string& in)
    {
        std::string out = in;
        for (char& c : out)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F)
                c = ' ';
        }
        if (out.size() > kMaxTitleBytes)
        {
            size_t cut = kMaxTitleBytes;
            // A continuation byte (10xxxxxx) at the cut belongs to a character that started earlier.
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
        }
        return out;
    }

    inline std::string Lower(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    // Case-insensitive order; equal names fall back to the exact bytes so the
    // output never depends on the input order.
    inline bool NameLess(const std::string& a, const std::string& b)
    {
        const std::string la = Lower(a), lb = Lower(b);
        return la != lb ? la < lb : a < b;
    }

    // The whole text: a plugin row for every plugin in `plugins`, each followed by
    // its features. A feature whose plugin is not in `plugins` is left out. Where
    // two plugins share a name the first one listed wins.
    inline std::string Build(uint32_t gameCL, const std::string& loader,
                             const std::vector<PluginRow>& plugins, const std::vector<FeatureRow>& features)
    {
        std::vector<PluginRow> rows;
        for (const PluginRow& p : plugins)
        {
            const std::string key = Lower(p.name);
            if (std::none_of(rows.begin(), rows.end(), [&](const PluginRow& r) { return Lower(r.name) == key; }))
                rows.push_back(p);
        }
        std::sort(rows.begin(), rows.end(), [](const PluginRow& a, const PluginRow& b) { return NameLess(a.name, b.name); });

        std::string text = "format\t1\nkind\tdeclared-features\ngame_cl\t" + std::to_string(gameCL) + "\nloader\t" +
                           SanitiseField(loader) + "\n";

        for (const PluginRow& p : rows)
        {
            const std::string key = Lower(p.name);
            std::vector<const FeatureRow*> mine;
            for (const FeatureRow& f : features)
                if (Lower(f.plugin) == key)
                    mine.push_back(&f);
            std::sort(mine.begin(), mine.end(), [](const FeatureRow* a, const FeatureRow* b)
                { return NameLess(a->id, b->id); });

            const std::string name = SanitiseField(p.name);
            text += "plugin\t" + name + "\t" + SanitiseField(p.version) + "\n";
            for (const FeatureRow* f : mine)
                text += "feature\t" + name + "\t" + SanitiseField(f->id) + "\t" + (f->on ? "on" : "off") + "\t" +
                        SanitiseTitle(f->title) + "\n";
        }
        return text;
    }

    // snprintf-style copy for a caller's buffer: returns the full length of
    // `text`, writes it NUL-terminated and cut to `size`; with a null buf or a
    // size of 0 nothing is written (a measure-only call).
    inline int CopyCut(const std::string& text, char* buf, size_t size)
    {
        if (buf && size > 0)
        {
            const size_t n = text.size() < size - 1 ? text.size() : size - 1;
            text.copy(buf, n);
            buf[n] = '\0';
        }
        return static_cast<int>(text.size());
    }

    // Write `text` to `path` through a temp file beside it, then rename over the
    // old file, so a reader sees the old text or the new, never half. False,
    // with the temp file removed, when any step fails.
    inline bool WriteAtomic(const std::filesystem::path& path, const std::string& text)
    {
        std::filesystem::path temp = path;
        temp += ".tmp";
        std::error_code ec;
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out)
            {
                out.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
        }
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }
}
