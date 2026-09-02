#pragma once

// ---------------------------------------------------------------------------
// PluginHookReport -- what happened during each plugin's OnPluginLoadHooks.
//
// A plugin resolves its AOB patterns inside one event (OnPluginLoadHooks, see
// plugin_interface.h) rather than whenever it feels like it, and this module is
// what makes that worth doing: it opens a scan session around the event, records
// every named resolve that missed, and answers the one question the loader asks
// when the event returns -- may this plugin load at all?
//
// A required miss is fatal. The plugin manager skips PluginInit, frees the DLL
// and leaves the record marked, so a plugin whose pattern stopped matching after
// a game update does nothing at all rather than detouring whatever now lives at
// the address it guessed.
//
// What survives the session is the report: one entry per plugin that had any
// problem, kept for the life of the process so the client can show it in a popup
// (UI/hook_failure_window.cpp) and both builds can print it from the console
// (`hookfailures`). Reports are keyed by plugin, so reloading a plugin replaces
// its entry instead of appending a second copy.
//
// Threading: every function takes the lock. Sessions are opened from the plugin
// manager while it holds g_pluginLock, so only one is ever open, but a plugin
// that scans from a thread it spawned during the event still lands somewhere
// sane -- and a call with no session open is rejected rather than recorded.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

struct IPluginSelf;

namespace PluginHookReport
{
    // One thing that did not resolve.
    struct Failure
    {
        std::string hookName;  // name the plugin gave it
        std::string detail;    // the pattern that missed, or the plugin's own message
        bool        fatal;     // true = required; false = optional/warning
    };

    // Everything one plugin reported during its last scan session.
    struct PluginReport
    {
        std::string          plugin;    // PluginInfo name, or the DLL file name if unknown
        std::string          file;      // bare DLL file name
        bool                 refused;   // true = fatal failures, plugin was not loaded
        int                  resolved;  // patterns that DID resolve, for context
        std::vector<Failure> failures;
    };

    // --- Session, driven by the plugin manager ---------------------------------

    // Open the scan session for one plugin. self is the identity every
    // IPluginHookScanner call must pass back. Any previous report for this
    // plugin is dropped now, so a reload replaces rather than accumulates.
    void BeginSession(const IPluginSelf* self, const char* pluginName, const char* fileName);

    // Close the session and commit its failures to the report list (nothing is
    // committed when the plugin resolved everything cleanly). refusedOut, when
    // non-null, receives true if the plugin must not be loaded.
    void EndSession(const IPluginSelf* self, bool* refusedOut);

    // Record a crash inside OnPluginLoadHooks against the open session. Fatal.
    void RecordSessionCrash(const IPluginSelf* self, const char* detail);

    // --- Recording, called by the IPluginHookScanner wrapper -------------------

    // False (and an error logged) when self has no session open -- which is what
    // stops a plugin from stashing the scanner table and using it later.
    bool HasSession(const IPluginSelf* self);

    void RecordResolved(const IPluginSelf* self);
    void RecordFailure(const IPluginSelf* self, const char* hookName, const char* detail, bool fatal);
    bool SessionHasFatal(const IPluginSelf* self);

    // --- Report, for the UI and the console -----------------------------------

    // Copy of every plugin report recorded this session.
    std::vector<PluginReport> Snapshot();

    // Number of plugins with any recorded problem, and the subset that were
    // refused. Cheap enough to call per UI frame.
    int GetReportedPluginCount();
    int GetRefusedPluginCount();

    // The whole report as plain text: what the clipboard button copies, what the
    // console prints, and what a user can paste to a plugin author.
    std::string BuildReportText();

    // Bumped every time the report list changes -- a session committing
    // failures, or a reload dropping a plugin's old entry. UI that caches
    // anything derived from Snapshot() stores this next to the cache and
    // rebuilds when it moves, rather than copying every string each frame.
    unsigned GetGeneration();

#ifdef _DEBUG
    // Add a fabricated report, for the F2 menu's test button. Debug builds only.
    void InjectTestReport(const PluginReport& report);
#endif
}
