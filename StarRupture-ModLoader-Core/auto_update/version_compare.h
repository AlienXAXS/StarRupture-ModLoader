#pragma once

#include <cstring>
#include <string>
#include <vector>

// Version tags as the release workflows write them, compared by number. Used
// by the Core's update checks and by the standalone updater, so the banner,
// the plugin pass and the updater agree on what is newer.
namespace AutoUpdateVersion
{
    struct ParsedVersion
    {
        int  parts[4] = { 0, 0, 0, 0 };
        bool valid = false;
        bool hasSuffix = false; // e.g. "-preview" / "-beta.1"
    };

    // Accepts "v1.15.4", "1.15.2.0", "v1.16.0-beta.1", etc.
    inline ParsedVersion ParseVersion(const char* s)
    {
        ParsedVersion v;
        if (!s || !*s)
            return v;

        const char* p = s;
        if (*p == 'v' || *p == 'V')
            ++p;

        int idx = 0;
        while (idx < 4)
        {
            if (*p < '0' || *p > '9')
                break;
            long val = 0;
            while (*p >= '0' && *p <= '9')
            {
                val = val * 10 + (*p - '0');
                if (val > 1000000) val = 1000000;
                ++p;
            }
            v.parts[idx++] = static_cast<int>(val);
            if (*p == '.')
                ++p;
            else
                break;
        }

        v.valid = (idx >= 2); // require at least major.minor
        v.hasSuffix = (*p == '-' || *p == '+');
        return v;
    }

    // Returns >0 if a is newer than b, 0 if equal, <0 if older (numeric only).
    // A suffix is ignored: 1.23.0-rc1 and 1.23.0 are equal, so a pre-release
    // is never offered its own release and never offered an older one.
    inline int CompareVersions(const ParsedVersion& a, const ParsedVersion& b)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (a.parts[i] != b.parts[i])
                return a.parts[i] < b.parts[i] ? -1 : 1;
        }
        return 0;
    }

    // Whether a version read from the installed DLL replaces the one stored in
    // update_state.ini: it parses, and the stored one is missing, does not
    // parse or is older by number.
    inline bool InstalledReplacesStored(const char* installed, const char* stored)
    {
        const ParsedVersion in = ParseVersion(installed);
        if (!in.valid)
            return false;
        const ParsedVersion was = ParseVersion(stored);
        return !was.valid || CompareVersions(in, was) > 0;
    }

    // Whether a release is newer than the installed version: by number, and
    // by plain difference when either does not parse.
    inline bool ReleaseIsNewer(const char* remote, const char* installed)
    {
        const ParsedVersion r = ParseVersion(remote);
        const ParsedVersion i = ParseVersion(installed);
        if (r.valid && i.valid)
            return CompareVersions(r, i) > 0;
        return std::strcmp(remote ? remote : "", installed ? installed : "") != 0;
    }

    // What the plugin update pass does with a release.
    //   stored   the version update_state.ini holds for the plugin ("" if none)
    //   onDisk   the version the installed DLL reports ("" if it could not be read)
    //   remote   the version the plugin's manifest offers
    // installed is the newer of stored and onDisk, so a build put in by hand,
    // newer than what the loader last wrote down, is never taken for the older
    // one. download is true only for a release newer than that; with no
    // installed version known there is nothing to compare against, so never.
    struct PluginUpdateDecision
    {
        std::string installed;
        bool        download = false;
    };

    inline PluginUpdateDecision DecidePluginUpdate(const char* stored, const char* onDisk, const char* remote)
    {
        PluginUpdateDecision d;
        d.installed = InstalledReplacesStored(onDisk, stored) ? onDisk : (stored ? stored : "");
        d.download  = !d.installed.empty() && ReleaseIsNewer(remote, d.installed.c_str());
        return d;
    }

    // ---- Release notes per version ---------------------------------------
    //
    // A manifest's "release_notes" holds one section per release, newest first,
    // each under a header line "## <version>" (the release tag), the section's
    // lines below it:
    //
    //     ## v1.23.1
    //     - a change
    //
    //     ## v1.23.0
    //     - another change
    //
    // The window lists the sections newer than the version the player had.

    // A line "## <version>": the marker, one version token that parses, and
    // nothing else ("## 2.0 is great" is a note). label gets the version as
    // written.
    inline bool ParseNotesHeader(const std::string& line, std::string& label, ParsedVersion& ver)
    {
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line.compare(b, 3, "## ") != 0)
            return false;
        const size_t s0 = line.find_first_not_of(" \t", b + 3);
        if (s0 == std::string::npos)
            return false;
        const size_t e = line.find_last_not_of(" \t");
        label = line.substr(s0, e - s0 + 1);
        if (label.find_first_of(" \t") != std::string::npos)
            return false;
        ver = ParseVersion(label.c_str());
        return ver.valid;
    }

    struct VersionedNotes
    {
        bool        versioned = false; // the text has at least one valid header
        std::string text;              // the selected lines; headers kept when more than one section is selected
    };

    // The sections of `text` newer than `previous` and no newer than `current`.
    // A previous version that is missing or does not parse selects the current
    // version's section only (and, with no usable current either, the first
    // section). A previous version at or above every section selects nothing.
    // Lines before the first header are ignored; empty sections and blank
    // lines are dropped. With a single section selected its header is left out,
    // so the lines read as a plain list.
    inline VersionedNotes SelectVersionedNotes(const char* text, const char* previous, const char* current)
    {
        struct Section { std::string label; ParsedVersion ver; std::vector<std::string> lines; };
        std::vector<Section> sections;

        const std::string all = text ? text : "";
        for (size_t pos = 0; pos <= all.size();)
        {
            size_t end = all.find('\n', pos);
            if (end == std::string::npos)
                end = all.size();
            std::string line = all.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            Section hdr;
            if (ParseNotesHeader(line, hdr.label, hdr.ver))
            {
                sections.push_back(std::move(hdr));
                continue;
            }
            if (sections.empty() || line.find_first_not_of(" \t") == std::string::npos)
                continue;
            sections.back().lines.push_back(std::move(line));
        }

        VersionedNotes r;
        r.versioned = !sections.empty();

        const ParsedVersion prev = ParseVersion(previous);
        const ParsedVersion cur  = ParseVersion(current);
        std::vector<const Section*> picked;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const Section& s = sections[i];
            if (s.lines.empty())
                continue;
            if (cur.valid && CompareVersions(s.ver, cur) > 0)
                continue;
            const bool wanted = prev.valid ? CompareVersions(s.ver, prev) > 0
                                           : (cur.valid ? CompareVersions(s.ver, cur) == 0 : i == 0);
            if (wanted)
                picked.push_back(&s);
        }

        for (const Section* s : picked)
        {
            if (picked.size() > 1)
                r.text += (r.text.empty() ? "## " : "\n## ") + s->label;
            for (const std::string& l : s->lines)
                r.text += (r.text.empty() ? "" : "\n") + l;
        }
        return r;
    }

    // The text for an update window row: the selected sections of releaseNotes
    // when it is versioned and anything is selected, else notes (a manifest
    // without the per-version field, or one that selects nothing).
    inline std::string NotesForUpdate(const char* releaseNotes, const char* notes, const char* previous, const char* current)
    {
        const VersionedNotes v = SelectVersionedNotes(releaseNotes, previous, current);
        if (!v.text.empty())
            return v.text;
        return notes ? notes : "";
    }

    // ---- The loader's previous version ------------------------------------
    //
    // The updater that installs a loader release is the one already installed,
    // so a loader first running after an update cannot count on state written
    // by its own updater. The updater logs "Update available: A -> B" and, on
    // every boot, "Installed version (...): A" to ModLoader\Logs\AutoUpdate.log
    // (the previous boots' logs are kept beside it), and those lines are what
    // the first run can read.

    inline std::string TrimToken(std::string v)
    {
        const size_t b = v.find_first_not_of(" \t\r");
        if (b == std::string::npos)
            return {};
        v.erase(0, b);
        v.erase(v.find_last_not_of(" \t\r") + 1);
        return v;
    }

    // The text after `marker` in each line of `log` that has it, in order.
    inline std::vector<std::string> LogLinesAfter(const char* log, const char* marker)
    {
        std::vector<std::string> out;
        const std::string all = log ? log : "";
        for (size_t pos = 0; pos < all.size();)
        {
            size_t end = all.find('\n', pos);
            if (end == std::string::npos)
                end = all.size();
            const std::string line = all.substr(pos, end - pos);
            pos = end + 1;
            const size_t p = line.find(marker);
            if (p != std::string::npos)
                out.push_back(line.substr(p + std::strlen(marker)));
        }
        return out;
    }

    // "A" of the last "Update available: A -> B" line whose B is `target` by number.
    inline std::string PreviousFromUpdateLine(const char* log, const char* target)
    {
        const ParsedVersion tv = ParseVersion(target);
        std::string found;
        for (const std::string& rest : LogLinesAfter(log, "Update available: "))
        {
            const size_t arrow = rest.find(" -> ");
            if (arrow == std::string::npos)
                continue;
            const std::string a = TrimToken(rest.substr(0, arrow));
            const std::string b = TrimToken(rest.substr(arrow + 4));
            const ParsedVersion av = ParseVersion(a.c_str());
            const ParsedVersion bv = ParseVersion(b.c_str());
            if (av.valid && bv.valid && tv.valid && CompareVersions(bv, tv) == 0 && CompareVersions(av, bv) < 0)
                found = a;
        }
        return found;
    }

    // "A" of the last "Installed version (...): A" line.
    inline std::string PreviousFromInstalledLine(const char* log)
    {
        std::string found;
        for (const std::string& rest : LogLinesAfter(log, "Installed version ("))
        {
            const size_t close = rest.find("): ");
            if (close == std::string::npos)
                continue;
            const std::string v = TrimToken(rest.substr(close + 3));
            if (ParseVersion(v.c_str()).valid)
                found = v;
        }
        return found;
    }

    // The version the player had before `running`, best source first:
    //   1. pending: the previous version an earlier launch resolved and kept in
    //      update_state.ini until the notes were shown, when older than
    //      `running` (a launch that quit before the window opened loses nothing,
    //      and a newer release arriving meanwhile lists the unshown ones too).
    //      A record of `running` or newer comes first: its notes were shown, and
    //      a pending value left behind by a crash after that is stale;
    //   2. recorded: what update_state.ini says notes were last shown for
    //      (written by this loader from 1.23.1 on);
    //   3. the current boot's updater log, when it installed `running`;
    //   4. the previous boot's updater log (a manual update leaves no line in
    //      the current one), when it names an older version.
    // "" when none of them does (a fresh install, or logs gone): the caller then
    // lists the running version's notes only.
    inline std::string ResolvePreviousVersion(const char* pending, const char* recorded, const char* currentLog,
                                              const char* previousLog, const char* running)
    {
        const ParsedVersion pv = ParseVersion(pending);
        const ParsedVersion rv = ParseVersion(running);
        const ParsedVersion rec = ParseVersion(recorded);
        const bool shownAlready = rec.valid && rv.valid && CompareVersions(rec, rv) >= 0;
        if (!shownAlready && pv.valid && rv.valid && CompareVersions(pv, rv) < 0)
            return pending;
        if (rec.valid)
            return recorded;
        std::string a = PreviousFromUpdateLine(currentLog, running);
        if (!a.empty())
            return a;
        a = PreviousFromInstalledLine(previousLog);
        const ParsedVersion av = ParseVersion(a.c_str());
        const ParsedVersion now = ParseVersion(running);
        if (av.valid && now.valid && CompareVersions(av, now) < 0)
            return a;
        return {};
    }

    // What the loader does with the release notes of the manifest it fetched.
    //   running       the version this loader is ("" on a dev build)
    //   manifestTag   the manifest's build_tag ("" if the fetch failed)
    //   previous      the version the player had before ("" if unknown)
    //   releaseNotes  the manifest's "release_notes" ("" if absent)
    //   notes         the manifest's "notes", used when release_notes gives nothing
    // The notes are shown once, for the release that is running: the manifest
    // names that version by number (a suffix such as -rc1 is ignored) and the
    // previous version is unknown or older. The text is the sections newer than
    // the previous version (see SelectVersionedNotes). A newer manifest is a
    // release this loader is not yet, so nothing is shown or recorded.
    struct LoaderNotesDecision
    {
        bool        show = false;
        std::string text;
    };

    inline LoaderNotesDecision DecideLoaderNotes(const char* running, const char* manifestTag, const char* previous,
                                                 const char* releaseNotes, const char* notes)
    {
        LoaderNotesDecision d;
        const ParsedVersion now = ParseVersion(running);
        const ParsedVersion rel = ParseVersion(manifestTag);
        if (!now.valid || !rel.valid || CompareVersions(rel, now) != 0)
            return d;

        const ParsedVersion was = ParseVersion(previous);
        if (was.valid && CompareVersions(was, now) >= 0)
            return d;

        const std::string text = NotesForUpdate(releaseNotes, notes, previous, manifestTag);
        if (text.find_first_not_of(" \t\r\n") == std::string::npos)
            return d;

        d.show = true;
        d.text = text;
        return d;
    }
}
