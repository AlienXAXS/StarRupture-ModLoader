#include "plugins/plugin_hook_report.h"

#include "plugins/host_rules.h"
#include "plugins/plugin_interface.h"
#include "logging/logger.h"
#include "core/version_check.h"
#include "core/unknown_build.h"
#include "utils/game_thread_dispatch.h"

#include <windows.h>
#include <algorithm>
#include <mutex>
#include <set>

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

#if defined(MODLOADER_CLIENT_BUILD)
#define MODLOADER_BUILD_KIND "client"
#elif defined(MODLOADER_SERVER_BUILD)
#define MODLOADER_BUILD_KIND "server"
#else
#define MODLOADER_BUILD_KIND "generic"
#endif

namespace PluginHookReport
{
    namespace
    {
        // A feature a plugin declared in this session (IPluginHookScanner::
        // DeclareFeature). Its keys are matched by RecordFailure.
        struct Declared
        {
            std::string              id;
            std::string              title;
            std::vector<std::string> keys;
            std::vector<std::string> configKeys;
            bool*                    enabled = nullptr;   // the plugin's flag; written only while the session is open
        };

        struct Session
        {
            const IPluginSelf*    self;
            PluginReport          report;
            bool                  nameShared = false;
            std::vector<Declared> declared;
            std::set<std::string> asked;      // every key a request named, resolved or not
            std::vector<PendingKey> pending;  // committed by EndSession unless the plugin is refused
            std::vector<AbsentKey>  absent;   // likewise
        };

        // What a feature that is off says, by why it went off. The log line, the
        // config page and the failure window all read it from FeatureOff::reason.
        const char* FeatureOffReason(Cause cause)
        {
            switch (cause)
            {
            case Cause::Plugin:  return "the plugin reported a problem with something it needs";
            case Cause::Request: return "the plugin asked for something it needs in a way the loader could not use";
            default:             return "this game build changed something it needs";
            }
        }

        // A feature of a loaded plugin, kept from EndSession until the plugin
        // is unloaded or reloaded (see the feature catalog in the header).
        struct CatalogEntry
        {
            const IPluginSelf*       self = nullptr;   // identity of the plugin; a name two plugins share cannot mix entries
            std::string              plugin;
            std::string              file;
            bool                     preload = false;
            std::string              id;
            std::string              title;
            std::vector<std::string> configKeys;
            bool*                    enabled = nullptr;   // the plugin's flag; valid until ForgetFeatures
            bool                     hostOff = false;     // the server's list has switched it off
        };

        std::mutex                s_mutex;
        std::vector<CatalogEntry> s_catalog;
        HostRules::DenySet        s_deny;           // what is denied now: both sources together; outlives the plugins it names
        HostRules::DenySources    s_sources;        // the console's list and the joined server's, kept apart
        std::string               s_hostMessage;    // text that goes with the console's list
        std::string               s_remoteMessage;  // text the joined server sent with its rules
        Session                   s_session{};      // self == nullptr when closed
        std::vector<PluginReport> s_reports;
        std::vector<PendingKey>   s_pending;        // keys waiting for a class, of loaded plugins
        std::vector<AbsentKey>    s_absent;         // keys the running build does not have, of loaded plugins
        unsigned                  s_generation = 0;
        unsigned                  s_commitSerial = 0;

        // Caller must hold s_mutex. Null when self is not the plugin whose
        // session is currently open -- including when no session is open at all,
        // which is what a plugin calling the scanner from PluginInit looks like.
        Session* FindSession(const IPluginSelf* self)
        {
            if (!self || s_session.self != self)
                return nullptr;
            return &s_session;
        }

        // Failure details grew multi-line in v68: a non-unique pattern reports
        // every match with the function it landed in. Writing that as a single
        // log call puts raw line breaks inside one line and ruins the log's
        // format, so split it here -- the one place every failure line goes
        // through.
        void LogDetailLines(const std::string& detail)
        {
            size_t start = 0;
            while (start <= detail.size())
            {
                const size_t end = detail.find("\r\n", start);
                const std::string line = detail.substr(
                    start, end == std::string::npos ? std::string::npos : end - start);

                if (!line.empty())
                    ModLoaderLogger::LogError(L"[HookScan]     %S", line.c_str());

                if (end == std::string::npos)
                    break;
                start = end + 2;
            }
        }

        bool SameName(const std::string& a, const std::string& b)
        {
            return _stricmp(a.c_str(), b.c_str()) == 0;
        }

        // Whether the running game build has the feature switched off: a key it
        // lists missed (a FeatureOff in the plugin's report), is waiting for a
        // class, or is not in this build. Worked out from the lists each time,
        // never cached, so a late resolve is seen the moment it happens.
        // `reason`, when non-null, receives one plain line. Caller holds s_mutex.
        bool BuildOffLocked(const std::string& plugin, bool preload, const std::string& id, std::string* reason)
        {
            for (const PluginReport& r : s_reports)
            {
                if (r.preload != preload || !SameName(r.plugin, plugin))
                    continue;
                for (const FeatureOff& f : r.featuresOff)
                    if (!f.host && f.id == id)
                    {
                        if (reason) *reason = f.reason;
                        return true;
                    }
            }
            for (const PendingKey& p : s_pending)
                if (SameName(p.plugin, plugin) && p.featureId == id)
                {
                    if (reason) *reason = p.refused ? p.detail : "waiting for a game class that is not loaded yet";
                    return true;
                }
            for (const AbsentKey& a : s_absent)
                if (SameName(a.plugin, plugin) && a.featureId == id)
                {
                    if (reason) *reason = "this game version does not have it";
                    return true;
                }
            return false;
        }

        const char* const kHostOffReason = "turned off by this server";

        // The plugin's report, or null. Caller holds s_mutex.
        PluginReport* FindReportLocked(const std::string& plugin, bool preload)
        {
            for (PluginReport& r : s_reports)
                if (r.preload == preload && SameName(r.plugin, plugin))
                    return &r;
            return nullptr;
        }

        // Make sure the plugin's report lists the feature as turned off by the
        // server, making a report for a plugin that has none. The report this
        // makes is not a failure and does not touch the commit serial, so the
        // failure window stays shut. True if a line was added. Caller holds s_mutex.
        bool EnsureHostOffLocked(const CatalogEntry& e)
        {
            PluginReport* r = FindReportLocked(e.plugin, e.preload);
            if (!r)
            {
                PluginReport made;
                made.plugin   = e.plugin;
                made.file     = e.file;
                made.refused  = false;
                made.resolved = 0;
                made.preload  = e.preload;
                s_reports.push_back(std::move(made));
                r = &s_reports.back();
            }
            for (const FeatureOff& f : r->featuresOff)
                if (f.host && f.id == e.id)
                    return false;

            FeatureOff off;
            off.id         = e.id;
            off.title      = e.title;
            off.reason     = kHostOffReason;
            off.configKeys = e.configKeys;
            off.host       = true;
            r->featuresOff.push_back(std::move(off));
            return true;
        }

        // Take the server's line for the feature out of the plugin's report, and
        // the report itself when that leaves nothing in it (one made for that
        // line alone). True if a line was removed. Caller holds s_mutex.
        bool RemoveHostOffLocked(const std::string& plugin, bool preload, const std::string& id)
        {
            for (size_t i = 0; i < s_reports.size(); ++i)
            {
                PluginReport& r = s_reports[i];
                if (r.preload != preload || !SameName(r.plugin, plugin))
                    continue;
                const auto it = std::remove_if(r.featuresOff.begin(), r.featuresOff.end(),
                    [&](const FeatureOff& f) { return f.host && f.id == id; });
                if (it == r.featuresOff.end())
                    return false;
                r.featuresOff.erase(it, r.featuresOff.end());
                if (r.failures.empty() && r.featuresOff.empty())
                    s_reports.erase(s_reports.begin() + static_cast<ptrdiff_t>(i));
                return true;
            }
            return false;
        }

        // Bring the catalog features of `plugin` (every plugin when null) in line
        // with the server's list. A feature goes off when it is on the list and
        // the build has it on; it comes back only when it is off for the server
        // and the build has it on. A feature that is already off for the server
        // gets its flag written false again, which is what a plugin that wrote
        // the flag after a late resolve needs. The report line is never added
        // under the commit serial. True if the report list changed. Game thread:
        // it writes the plugins' flags. Caller holds s_mutex.
        bool ApplyOverlayLocked(const std::string* plugin)
        {
            bool changed = false;
            for (CatalogEntry& e : s_catalog)
            {
                if (plugin && !SameName(e.plugin, *plugin))
                    continue;

                const bool buildOn = !BuildOffLocked(e.plugin, e.preload, e.id, nullptr);
                const bool hostOff = buildOn && HostRules::Contains(s_deny, e.plugin, e.id);

                if (hostOff)
                {
                    if (e.enabled)
                        *e.enabled = HostRules::EffectiveOn(buildOn, hostOff);
                    if (!e.hostOff)
                    {
                        e.hostOff = true;
                        ModLoaderLogger::LogInfo(L"[Map] %S: feature \"%S\" turned off by this server",
                            e.plugin.c_str(), e.title.c_str());
                    }
                    if (EnsureHostOffLocked(e))
                        changed = true;
                }
                else if (e.hostOff)
                {
                    e.hostOff = false;
                    if (e.enabled && buildOn)
                        *e.enabled = HostRules::EffectiveOn(buildOn, hostOff);
                    if (RemoveHostOffLocked(e.plugin, e.preload, e.id))
                        changed = true;
                    if (buildOn)
                        ModLoaderLogger::LogInfo(L"[Map] %S: feature \"%S\" turned back on",
                            e.plugin.c_str(), e.title.c_str());
                }
            }
            return changed;
        }

        // Calls log(first, keys) once per feature in `entries`: `first` is its
        // first entry and `keys` the names of all its entries, "a, b".
        template <typename Entry, typename Log>
        void ForEachFeature(const std::vector<Entry>& entries, Log log)
        {
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const Entry& first = entries[i];
                if (std::any_of(entries.begin(), entries.begin() + static_cast<ptrdiff_t>(i),
                        [&](const Entry& e) { return e.featureId == first.featureId; }))
                    continue;
                std::string keys;
                for (const Entry& e : entries)
                    if (e.featureId == first.featureId)
                        keys += (keys.empty() ? "" : ", ") + e.key;
                log(first, keys);
            }
        }

        // Whether `configKey` ("Section.Key", or "Section.*" for a whole
        // section) names section.key.
        bool ConfigKeyMatches(const std::string& configKey, const char* section, const char* key)
        {
            const std::string s = section ? section : "";
            const std::string k = key ? key : "";
            if (configKey.size() > 2 && configKey.compare(configKey.size() - 2, 2, ".*") == 0)
                return SameName(configKey.substr(0, configKey.size() - 2), s);
            return SameName(configKey, s + "." + k);
        }

        void RejectOutOfSession(const IPluginSelf* self, const char* what)
        {
            ModLoaderLogger::LogError(
                L"[HookScan] Plugin '%S' called %S outside OnPluginLoadHooks -- rejected. "
                L"Pattern scanning is only available inside that event; resolve there and "
                L"install from PluginInit.",
                (self && self->name) ? self->name : "(unknown)", what ? what : "the scanner");
        }
    }

    void BeginSession(const IPluginSelf* self, const char* pluginName, const char* fileName,
                      bool preload, bool nameShared)
    {
        if (!self) return;

        std::lock_guard<std::mutex> lock(s_mutex);

        // A reload: the old load's catalog entries go with its DLL. The plugin
        // manager has already called ForgetFeatures before freeing it; this
        // keeps the catalog right for any caller that did not.
        s_catalog.erase(std::remove_if(s_catalog.begin(), s_catalog.end(),
            [&](const CatalogEntry& e) { return e.self == self; }), s_catalog.end());

        s_session = Session{};
        s_session.self           = self;
        s_session.report.plugin  = (pluginName && pluginName[0]) ? pluginName
                                 : (fileName ? fileName : "(unknown)");
        s_session.report.file    = fileName ? fileName : "";
        s_session.report.refused = false;
        s_session.report.resolved = 0;
        s_session.report.preload = preload;
        s_session.nameShared     = nameShared;

        // The same for the keys it was waiting for.
        const size_t pendingBefore = s_pending.size();
        s_pending.erase(std::remove_if(s_pending.begin(), s_pending.end(),
            [&](const PendingKey& p) { return SameName(p.plugin, s_session.report.plugin); }), s_pending.end());
        if (s_pending.size() != pendingBefore)
            ++s_generation;
        const size_t absentBefore = s_absent.size();
        s_absent.erase(std::remove_if(s_absent.begin(), s_absent.end(),
            [&](const AbsentKey& a) { return SameName(a.plugin, s_session.report.plugin); }), s_absent.end());
        if (s_absent.size() != absentBefore)
            ++s_generation;

        // Drop any report from a previous load of this plugin: a reload that now
        // resolves cleanly must not leave the old failures on screen.
        for (size_t i = 0; i < s_reports.size(); ++i)
        {
            if (s_reports[i].preload == preload &&
                _stricmp(s_reports[i].plugin.c_str(), s_session.report.plugin.c_str()) == 0)
            {
                s_reports.erase(s_reports.begin() + static_cast<ptrdiff_t>(i));
                ++s_generation;
                break;
            }
        }
    }

    void EndSession(const IPluginSelf* self, bool* refusedOut)
    {
        if (refusedOut) *refusedOut = false;

        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
            return;

        for (const Declared& d : session->declared)
            for (const std::string& key : d.keys)
                if (!session->asked.count(key))
                    ModLoaderLogger::LogWarn(L"[Map] %S: feature \"%S\" lists \"%S\", which no request asked for",
                        session->report.plugin.c_str(), d.title.c_str(), key.c_str());

        PluginReport report = std::move(session->report);
        const std::string pluginName = report.plugin;
        std::vector<PendingKey> pending = std::move(session->pending);
        std::vector<AbsentKey>  absent  = std::move(session->absent);
        std::vector<Declared>   declared = std::move(session->declared);
        s_session = Session{};

        // A plugin that loads keeps its features in the catalog until it is
        // unloaded. A refused one is never run, so it has nothing to keep.
        if (report.failures.empty())
            for (Declared& d : declared)
            {
                CatalogEntry e;
                e.self       = self;
                e.plugin     = report.plugin;
                e.file       = report.file;
                e.preload    = report.preload;
                e.id         = std::move(d.id);
                e.title      = std::move(d.title);
                e.configKeys = std::move(d.configKeys);
                e.enabled    = d.enabled;
                s_catalog.push_back(std::move(e));
            }

        // Keys waiting for a class are kept unless the plugin is refused. They
        // are not failures: the log says so at Info, and no popup opens.
        if (report.failures.empty() && !pending.empty())
        {
            ForEachFeature(pending, [&](const PendingKey& first, const std::string& keys)
            {
                ModLoaderLogger::LogInfo(L"[Map] %S: feature \"%S\" is waiting for a game class that is not loaded yet: %S",
                    report.plugin.c_str(), first.featureTitle.c_str(), keys.c_str());
            });
            s_pending.insert(s_pending.end(), pending.begin(), pending.end());
            ++s_generation;
        }

        // Keys the running build does not have are kept the same way: information, not a failure.
        if (report.failures.empty() && !absent.empty())
        {
            ForEachFeature(absent, [&](const AbsentKey& first, const std::string& keys)
            {
                ModLoaderLogger::LogInfo(L"[Map] %S: feature \"%S\" is not in this game version: %S",
                    report.plugin.c_str(), first.featureTitle.c_str(), keys.c_str());
            });
            s_absent.insert(s_absent.end(), absent.begin(), absent.end());
            ++s_generation;
        }

        if (report.failures.empty() && report.featuresOff.empty())
        {
            if (report.resolved > 0)
                ModLoaderLogger::LogInfo(L"[HookScan] %S: %d pattern(s) resolved, no failures",
                    report.plugin.c_str(), report.resolved);
            if (ApplyOverlayLocked(&pluginName))
                ++s_generation;
            return;
        }

        // A miss on a key no feature lists is fatal, and one is enough. An
        // optional resolve that came back empty still means the plugin is about
        // to run against a game it was not built for, and the loader cannot
        // tell which half of it that breaks. A feature turned off is the
        // plugin saying which half.
        report.refused = !report.failures.empty();

        if (refusedOut) *refusedOut = report.refused;

        for (const FeatureOff& f : report.featuresOff)
        {
            std::string keys;
            for (const FeatureMiss& m : f.misses)
                keys += (keys.empty() ? "" : ", ") + m.key;
            ModLoaderLogger::LogWarn(L"[Map] %S: feature \"%S\" is off: %S (%S)",
                report.plugin.c_str(), f.title.c_str(), keys.c_str(), f.reason.c_str());
        }

        if (!report.failures.empty())
        {
            ModLoaderLogger::LogError(L"[HookScan] ==========================================================");
            ModLoaderLogger::LogError(L"[HookScan] %S '%S' (%S): %d resolved, %zu failed",
                report.preload ? "Preload plugin" : "Plugin",
                report.plugin.c_str(), report.file.c_str(), report.resolved, report.failures.size());
            for (const Failure& f : report.failures)
            {
                ModLoaderLogger::LogError(L"[HookScan]   [%S] %S", f.required ? "required" : "optional", f.hookName.c_str());
                LogDetailLines(f.detail);
            }
            if (report.preload)
            {
                ModLoaderLogger::LogError(L"[HookScan] '%S' will NOT run -- it is unloaded and the game boots unaffected.",
                    report.plugin.c_str());
            }
            else
            {
                ModLoaderLogger::LogError(L"[HookScan] '%S' will NOT be loaded -- PluginInit will not be called.",
                    report.plugin.c_str());
            }
            ModLoaderLogger::LogError(L"[HookScan] ==========================================================");
        }

        s_reports.push_back(std::move(report));
        ++s_generation;
        ++s_commitSerial;

        // After the push, so the server's line joins the plugin's own report
        // (the commit serial above is already spent on the plugin's problems).
        ApplyOverlayLocked(&pluginName);
    }

    void RecordSessionCrash(const IPluginSelf* self, const char* detail)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
            return;

        Failure f;
        f.hookName = "OnPluginLoadHooks";
        f.detail   = detail ? detail : "crashed while resolving patterns";
        f.required = true;
        session->report.failures.push_back(std::move(f));
    }

    bool HasSession(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return FindSession(self) != nullptr;
    }

    void RecordResolved(const IPluginSelf* self, const char* name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (Session* session = FindSession(self))
        {
            session->report.resolved++;
            session->asked.insert(name ? name : "");
        }
    }

    void RecordFieldResolved(const IPluginSelf* self, const char* key)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (Session* session = FindSession(self))
            session->asked.insert(key ? key : "");
    }

    void RecordFailure(const IPluginSelf* self, const char* hookName, const char* detail, bool required,
                       Cause cause)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
        {
            RejectOutOfSession(self, "ReportFailure/ReportWarning");
            return;
        }

        const std::string name = (hookName && hookName[0]) ? hookName : "(unnamed)";
        session->asked.insert(name);

        // A key a declared feature lists turns that feature off instead of
        // refusing the plugin. Every feature that lists it goes off.
        bool covered = false;
        for (Declared& d : session->declared)
        {
            if (std::find(d.keys.begin(), d.keys.end(), name) == d.keys.end())
                continue;
            covered = true;

            auto off = std::find_if(session->report.featuresOff.begin(), session->report.featuresOff.end(),
                [&](const FeatureOff& o) { return o.id == d.id; });
            if (off == session->report.featuresOff.end())
            {
                FeatureOff o;
                o.id         = d.id;
                o.title      = d.title;
                o.reason     = FeatureOffReason(cause);
                o.configKeys = d.configKeys;
                session->report.featuresOff.push_back(std::move(o));
                off = session->report.featuresOff.end() - 1;
            }
            off->misses.push_back(FeatureMiss{ name, detail ? detail : "" });
            if (d.enabled)
                *d.enabled = false;
        }
        if (covered)
            return;

        Failure f;
        f.hookName = name;
        f.detail   = detail ? detail : "";
        f.required = required;
        session->report.failures.push_back(std::move(f));
    }

    void RecordPending(const IPluginSelf* self, const char* key, const char* detail)
    {
        std::unique_lock<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
        {
            RejectOutOfSession(self, "ResolveField");
            return;
        }

        const std::string name = (key && key[0]) ? key : "(unnamed)";
        session->asked.insert(name);

        bool covered = false;
        for (Declared& d : session->declared)
        {
            if (std::find(d.keys.begin(), d.keys.end(), name) == d.keys.end())
                continue;
            covered = true;
            if (d.enabled)
                *d.enabled = false;
            PendingKey p;
            p.plugin       = session->report.plugin;
            p.key          = name;
            p.detail       = detail ? detail : "";
            p.featureId    = d.id;
            p.featureTitle = d.title;
            p.configKeys   = d.configKeys;
            session->pending.push_back(std::move(p));
        }
        if (covered)
            return;

        // Nothing is switched off by a key no feature lists, so there is
        // nothing that could wait: it is a miss like any other.
        lock.unlock();
        RecordFailure(self, name.c_str(),
            (std::string(detail ? detail : "") + " (no feature lists this key, so it cannot wait for the class)").c_str(), true);
    }

    void LateResolved(const IPluginSelf* self, const char* key)
    {
        if (!self || !self->name || !key)
            return;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            const size_t before = s_pending.size();
            s_pending.erase(std::remove_if(s_pending.begin(), s_pending.end(),
                [&](const PendingKey& p) { return SameName(p.plugin, self->name) && p.key == key; }), s_pending.end());
            if (s_pending.size() == before)
                return;
            ++s_generation;
            if (s_deny.empty())
                return;
        }

        // The feature may be on the server's list. The plugin writes its own
        // `enabled` flag when this call returns, so the list is applied on the
        // next game tick, after that write.
        const std::string plugin = self->name;
        GameThreadDispatch::PostVoid([plugin] { ReapplyHostRules(plugin.c_str()); });
    }

    void LateRefused(const IPluginSelf* self, const char* key, const char* detail)
    {
        if (!self || !self->name || !key)
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        bool changed = false;
        for (PendingKey& p : s_pending)
            if (SameName(p.plugin, self->name) && p.key == key && (!p.refused || p.detail != (detail ? detail : "")))
            {
                p.refused = true;
                p.detail  = detail ? detail : "";
                changed   = true;
            }
        if (changed)
            ++s_generation;
    }

    std::vector<PendingKey> Pending()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_pending;
    }

    std::vector<FeatureInfo> Features()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<FeatureInfo> out;
        out.reserve(s_catalog.size());
        for (const CatalogEntry& e : s_catalog)
        {
            FeatureInfo info;
            info.plugin  = e.plugin;
            info.id      = e.id;
            info.title   = e.title;
            info.preload = e.preload;
            if (e.hostOff)
            {
                info.state  = FeatureInfo::State::OffServer;
                info.reason = kHostOffReason;
            }
            else if (BuildOffLocked(e.plugin, e.preload, e.id, &info.reason))
                info.state = FeatureInfo::State::OffBuild;
            out.push_back(std::move(info));
        }
        return out;
    }

    void ForgetFeatures(const IPluginSelf* self)
    {
        if (!self)
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        bool changed = false;
        std::vector<std::string> names;
        for (const CatalogEntry& e : s_catalog)
            if (e.self == self)
            {
                if (e.hostOff && RemoveHostOffLocked(e.plugin, e.preload, e.id))
                    changed = true;
                names.push_back(e.plugin);
            }
        s_catalog.erase(std::remove_if(s_catalog.begin(), s_catalog.end(),
            [&](const CatalogEntry& e) { return e.self == self; }), s_catalog.end());

        // Another loaded plugin that shares the name keeps its own lines.
        for (const std::string& name : names)
            if (ApplyOverlayLocked(&name))
                changed = true;
        if (changed)
            ++s_generation;
    }

    namespace
    {
        HostRules::DenySet ToDenySet(const std::vector<std::pair<std::string, std::string>>& deny)
        {
            HostRules::DenySet set;
            set.reserve(deny.size());
            for (const auto& d : deny)
                set.push_back(HostRules::RuleKey{ d.first, d.second });
            return HostRules::Normalize(set);
        }

        // Switch what is denied to the two sources together: features that came
        // onto the list go off, features that left it are handed back. Caller
        // holds s_mutex. Game thread.
        void ApplySourcesLocked()
        {
            HostRules::DenySet wanted = s_sources.Effective();
            const HostRules::DenyDiff diff = HostRules::DiffDeny(s_deny, wanted);
            s_deny = std::move(wanted);

            bool changed = false;
            for (const HostRules::DenySet* side : { &diff.added, &diff.removed })
                for (const HostRules::RuleKey& r : *side)
                    if (ApplyOverlayLocked(&r.plugin))
                        changed = true;
            if (changed)
                ++s_generation;
        }
    }

    void ApplyHostRules(const std::vector<std::pair<std::string, std::string>>& deny)
    {
        HostRules::DenySet wanted = ToDenySet(deny);
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sources.local = std::move(wanted);
        ApplySourcesLocked();
    }

    void ClearHostRules()
    {
        ApplyHostRules({});
        SetHostMessage(std::string());
    }

    void SetHostMessage(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_hostMessage = message;
    }

    std::string HostMessage()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_hostMessage;
    }

    void ApplyServerRules(const std::vector<std::pair<std::string, std::string>>& deny, const std::string& message)
    {
        HostRules::DenySet wanted = ToDenySet(deny);
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sources.remote = std::move(wanted);
        s_remoteMessage = message;
        ApplySourcesLocked();
    }

    void ClearServerRules()
    {
        ApplyServerRules({}, std::string());
    }

    std::vector<std::pair<std::string, std::string>> ServerDeny()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(s_sources.remote.size());
        for (const HostRules::RuleKey& r : s_sources.remote)
            out.emplace_back(r.plugin, r.id);
        return out;
    }

    std::string ServerMessage()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_remoteMessage;
    }

    void ReapplyHostRules(const char* plugin)
    {
        if (!plugin || !plugin[0])
            return;
        const std::string name = plugin;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (ApplyOverlayLocked(&name))
            ++s_generation;
    }

    std::vector<std::pair<std::string, std::string>> HostDeny()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(s_sources.local.size());
        for (const HostRules::RuleKey& r : s_sources.local)
            out.emplace_back(r.plugin, r.id);
        return out;
    }

    void RecordAbsent(const IPluginSelf* self, const char* key, const char* detail)
    {
        std::unique_lock<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
        {
            RejectOutOfSession(self, "ResolveField");
            return;
        }

        const std::string name = (key && key[0]) ? key : "(unnamed)";
        session->asked.insert(name);

        bool covered = false;
        for (Declared& d : session->declared)
        {
            if (std::find(d.keys.begin(), d.keys.end(), name) == d.keys.end())
                continue;
            covered = true;
            if (d.enabled)
                *d.enabled = false;
            if (std::any_of(session->absent.begin(), session->absent.end(),
                    [&](const AbsentKey& x) { return x.featureId == d.id && x.key == name; }))
                continue;
            AbsentKey a;
            a.plugin       = session->report.plugin;
            a.key          = name;
            a.featureId    = d.id;
            a.featureTitle = d.title;
            a.configKeys   = d.configKeys;
            session->absent.push_back(std::move(a));
        }
        if (covered)
            return;

        // No feature lists the key, so nothing can go off in its place: the
        // plugin cannot do without it, as for any other miss.
        lock.unlock();
        RecordFailure(self, name.c_str(),
            (std::string(detail ? detail : "") + " (no feature lists this key, so the plugin cannot do without it)").c_str(), true);
    }

    bool DeclareFeature(const IPluginSelf* self, const char* id, const char* title,
                        const std::vector<std::string>& keys, const std::vector<std::string>& configKeys,
                        bool* enabled)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
        {
            RejectOutOfSession(self, "DeclareFeature");
            return false;
        }
        if (!id || !id[0] || !title || !title[0] || keys.empty())
            return false;
        for (const Declared& d : session->declared)
            if (d.id == id)
                return false;

        Declared d;
        d.id         = id;
        d.title      = title;
        d.keys       = keys;
        d.configKeys = configKeys;
        d.enabled    = enabled;
        if (enabled)
            *enabled = true;
        session->declared.push_back(std::move(d));
        return true;
    }

    bool SessionNameShared(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const Session* session = FindSession(self);
        return session && session->nameShared;
    }

    bool SessionHasFailures(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
            return false;

        return !session->report.failures.empty();
    }

    unsigned GetGeneration()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_generation;
    }

    unsigned GetCommitSerial()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_commitSerial;
    }

    void AddReport(const PluginReport& report)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_reports.push_back(report);
        ++s_generation;
        ++s_commitSerial;
    }

    std::vector<PluginReport> Snapshot()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_reports;
    }

    int GetReportedPluginCount()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return static_cast<int>(s_reports.size());
    }

    int GetRefusedPluginCount()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        int count = 0;
        for (const PluginReport& r : s_reports)
            if (r.refused) count++;
        return count;
    }

    int GetFeaturesOffCount(const char* plugin, bool preload)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        int count = 0;
        for (const PluginReport& r : s_reports)
            if (plugin && r.preload == preload && SameName(r.plugin, plugin))
                count += static_cast<int>(r.featuresOff.size());
        return count;
    }

    std::string ConfigOffReason(const char* plugin, const char* section, const char* key)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const PluginReport& r : s_reports)
        {
            if (!plugin || !SameName(r.plugin, plugin))
                continue;
            for (const FeatureOff& f : r.featuresOff)
                for (const std::string& configKey : f.configKeys)
                    if (ConfigKeyMatches(configKey, section, key))
                        return f.title + " is off: " + f.reason;
        }
        for (const PendingKey& p : s_pending)
        {
            if (!plugin || !SameName(p.plugin, plugin))
                continue;
            for (const std::string& configKey : p.configKeys)
                if (ConfigKeyMatches(configKey, section, key))
                    return p.featureTitle + (p.refused ? " is off: " + p.detail
                        : std::string(" is waiting for the game to load something it needs"));
        }
        for (const AbsentKey& a : s_absent)
        {
            if (!plugin || !SameName(a.plugin, plugin))
                continue;
            for (const std::string& configKey : a.configKeys)
                if (ConfigKeyMatches(configKey, section, key))
                    return a.featureTitle + " is not in this game version";
        }
        return std::string();
    }

    std::string BuildReportText()
    {
        // Written for pasting into a bug report, so it leads with the two things
        // that decide whether a pattern was ever going to match -- which game
        // build this is and which loader build scanned it.
        char header[512]{};
        char gameVersion[128] = "unknown";
        {
            const std::wstring wide = GetGameVersionString();
            if (!wide.empty())
                WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, gameVersion,
                    static_cast<int>(sizeof(gameVersion)), "?", nullptr);
        }

        sprintf_s(header,
            "StarRupture ModLoader -- plugin hook failures\r\n"
            "Loader:       %s (%s build)\r\n"
            "Game version: %s\r\n",
            MODLOADER_BUILD_TAG, MODLOADER_BUILD_KIND, gameVersion);

        std::string text = header;
#ifdef MODLOADER_CLIENT_BUILD
        if (UnknownBuild::Active())
            text += "Unknown build: " + UnknownBuild::ReportLine(UnknownBuild::RunningCL(), UnknownBuild::NewestCL()) + "\r\n";
#endif
        text += "\r\n";

        std::lock_guard<std::mutex> lock(s_mutex);

        // Keys waiting for a game class that is not loaded yet are not failures.
        std::string waiting;
        for (const PendingKey& p : s_pending)
        {
            waiting += p.plugin + ": " + (p.refused ? "[refused] " : "[pending] ") + p.key + " (feature "
                + p.featureTitle + ")\r\n        " + p.detail + "\r\n";
        }
        if (!waiting.empty())
            waiting = "Waiting for a game class to load (not failures; the feature turns on when it does):\r\n"
                + waiting + "\r\n";

        // Keys the running build does not have: the feature does not apply to
        // this game version, and there is nothing to fix.
        std::string notInVersion;
        for (const AbsentKey& a : s_absent)
            notInVersion += a.plugin + ": " + a.featureTitle + " (" + a.key + ")\r\n";
        if (!notInVersion.empty())
            notInVersion = "Not in this game version (not failures; the feature does not apply to this build):\r\n"
                + notInVersion + "\r\n";

        if (s_reports.empty())
        {
            text += "No plugin reported a failed hook.\r\n";
            if (!waiting.empty() || !notInVersion.empty())
                text += "\r\n" + waiting + notInVersion;
            return text;
        }
        text += waiting + notInVersion;

        // Built by appending rather than formatting into a fixed buffer: a
        // detail line is a plugin's AOB pattern or its own message, and neither
        // has a length this code gets to assume.
        for (const PluginReport& r : s_reports)
        {
            text += r.preload ? "[preload] " : "";
            text += r.plugin;
            text += " (";
            text += r.file.empty() ? "?" : r.file;
            text += ") -- ";
            text += r.refused ? "NOT LOADED" : "loaded";
            if (!r.featuresOff.empty())
            {
                text += ", ";
                text += std::to_string(r.featuresOff.size());
                text += " feature(s) off";
            }
            text += "   [";
            text += std::to_string(r.resolved);
            text += " pattern(s) resolved]\r\n";

            for (const Failure& f : r.failures)
            {
                text += f.required ? "    [required] " : "    [optional] ";
                text += f.hookName;
                text += "\r\n        ";
                text += f.detail;
                text += "\r\n";
            }
            for (const FeatureOff& f : r.featuresOff)
            {
                text += "    [feature off] ";
                text += f.title;
                text += " (";
                text += f.id;
                text += "): ";
                text += f.reason;
                text += "\r\n";
                for (const FeatureMiss& m : f.misses)
                {
                    text += "        ";
                    text += m.key;
                    text += "\r\n            ";
                    text += m.detail;
                    text += "\r\n";
                }
            }
            text += "\r\n";
        }

        text += "A hook that no longer resolves usually means the game updated and the\r\n"
                "plugin needs a new build. A hook the plugin cannot do without stops the\r\n"
                "plugin loading; a hook that only one feature needs turns that feature off\r\n"
                "and the rest of the plugin runs. A pattern that matches MORE than once\r\n"
                "counts as unresolved too, because an address picked from several\r\n"
                "candidates is a guess. Send this to the plugin's author.\r\n";
        return text;
    }
}
