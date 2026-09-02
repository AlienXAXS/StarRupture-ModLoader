#include "plugins/plugin_hook_report.h"

#include "plugins/plugin_interface.h"
#include "logging/logger.h"
#include "core/version_check.h"

#include <windows.h>
#include <mutex>

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
        struct Session
        {
            const IPluginSelf* self;
            PluginReport       report;
        };

        std::mutex                s_mutex;
        Session                   s_session{};      // self == nullptr when closed
        std::vector<PluginReport> s_reports;
        unsigned                  s_generation = 0;

        // Caller must hold s_mutex. Null when self is not the plugin whose
        // session is currently open -- including when no session is open at all,
        // which is what a plugin calling the scanner from PluginInit looks like.
        Session* FindSession(const IPluginSelf* self)
        {
            if (!self || s_session.self != self)
                return nullptr;
            return &s_session;
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

    void BeginSession(const IPluginSelf* self, const char* pluginName, const char* fileName)
    {
        if (!self) return;

        std::lock_guard<std::mutex> lock(s_mutex);

        s_session = Session{};
        s_session.self           = self;
        s_session.report.plugin  = (pluginName && pluginName[0]) ? pluginName
                                 : (fileName ? fileName : "(unknown)");
        s_session.report.file    = fileName ? fileName : "";
        s_session.report.refused = false;
        s_session.report.resolved = 0;

        // Drop any report from a previous load of this plugin: a reload that now
        // resolves cleanly must not leave the old failures on screen.
        for (size_t i = 0; i < s_reports.size(); ++i)
        {
            if (_stricmp(s_reports[i].plugin.c_str(), s_session.report.plugin.c_str()) == 0)
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

        PluginReport report = std::move(session->report);
        s_session = Session{};

        if (report.failures.empty())
        {
            if (report.resolved > 0)
                ModLoaderLogger::LogInfo(L"[HookScan] %S: %d pattern(s) resolved, no failures",
                    report.plugin.c_str(), report.resolved);
            return;
        }

        for (const Failure& f : report.failures)
        {
            if (f.fatal) { report.refused = true; break; }
        }

        if (refusedOut) *refusedOut = report.refused;

        ModLoaderLogger::LogError(L"[HookScan] ==========================================================");
        ModLoaderLogger::LogError(L"[HookScan] Plugin '%S' (%S): %d resolved, %zu failed",
            report.plugin.c_str(), report.file.c_str(), report.resolved, report.failures.size());
        for (const Failure& f : report.failures)
        {
            ModLoaderLogger::LogError(L"[HookScan]   [%S] %S", f.fatal ? "REQUIRED" : "optional", f.hookName.c_str());
            ModLoaderLogger::LogError(L"[HookScan]     %S", f.detail.c_str());
        }
        if (report.refused)
            ModLoaderLogger::LogError(L"[HookScan] '%S' will NOT be loaded -- PluginInit will not be called.",
                report.plugin.c_str());
        else
            ModLoaderLogger::LogWarn(L"[HookScan] '%S' loads anyway; only optional patterns missed.",
                report.plugin.c_str());
        ModLoaderLogger::LogError(L"[HookScan] ==========================================================");

        s_reports.push_back(std::move(report));
        ++s_generation;
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
        f.fatal    = true;
        session->report.failures.push_back(std::move(f));
    }

    bool HasSession(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return FindSession(self) != nullptr;
    }

    void RecordResolved(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (Session* session = FindSession(self))
            session->report.resolved++;
    }

    void RecordFailure(const IPluginSelf* self, const char* hookName, const char* detail, bool fatal)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
        {
            RejectOutOfSession(self, "ReportFailure/ReportWarning");
            return;
        }

        Failure f;
        f.hookName = (hookName && hookName[0]) ? hookName : "(unnamed)";
        f.detail   = detail ? detail : "";
        f.fatal    = fatal;
        session->report.failures.push_back(std::move(f));
    }

    bool SessionHasFatal(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        Session* session = FindSession(self);
        if (!session)
            return false;

        for (const Failure& f : session->report.failures)
            if (f.fatal) return true;
        return false;
    }

    unsigned GetGeneration()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_generation;
    }

#ifdef _DEBUG
    void InjectTestReport(const PluginReport& report)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_reports.push_back(report);
        ++s_generation;
    }
#endif

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
            "Game version: %s\r\n\r\n",
            MODLOADER_BUILD_TAG, MODLOADER_BUILD_KIND, gameVersion);

        std::string text = header;

        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_reports.empty())
        {
            text += "No plugin reported a failed hook.\r\n";
            return text;
        }

        // Built by appending rather than formatting into a fixed buffer: a
        // detail line is a plugin's AOB pattern or its own message, and neither
        // has a length this code gets to assume.
        for (const PluginReport& r : s_reports)
        {
            text += r.plugin;
            text += " (";
            text += r.file.empty() ? "?" : r.file;
            text += ") -- ";
            text += r.refused ? "NOT LOADED" : "loaded with warnings";
            text += "   [";
            text += std::to_string(r.resolved);
            text += " pattern(s) resolved]\r\n";

            for (const Failure& f : r.failures)
            {
                text += f.fatal ? "    [required] " : "    [optional] ";
                text += f.hookName;
                text += "\r\n        ";
                text += f.detail;
                text += "\r\n";
            }
            text += "\r\n";
        }

        text += "A required hook that no longer resolves usually means the game updated\r\n"
                "and the plugin needs a new build. Send this to the plugin's author.\r\n";
        return text;
    }
}
