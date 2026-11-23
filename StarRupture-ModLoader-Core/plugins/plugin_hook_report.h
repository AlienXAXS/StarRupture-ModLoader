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
// A miss on a key that no declared feature lists is fatal -- required or
// optional, one is enough. The plugin manager skips PluginInit, frees the DLL
// and leaves the record marked, so a plugin whose pattern stopped matching
// after a game update does nothing at all rather than detouring whatever now
// lives at the address it guessed.
//
// A plugin can declare features (DeclareFeature): parts of it that can be off
// while the rest runs, each with the keys it needs. A miss on one of those keys
// is not fatal. It turns the feature off, records it in the report and lets the
// plugin load; the plugin is told through the feature's `enabled` flag, and the
// config page greys out the entries the feature drives (ConfigOffReason). The
// address is never used: an unresolved key stays unresolved.
//
// The required/optional flag survives only as a label on the failure line: it
// says how the plugin declared that resolve, and no longer decides anything.
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
#include <utility>
#include <vector>

struct IPluginSelf;

namespace PluginHookReport
{
    // One thing that did not resolve.
    struct Failure
    {
        std::string hookName;  // name the plugin gave it
        std::string detail;    // the pattern that missed, or the plugin's own message
        bool        required;  // how the plugin declared it -- a label, not a verdict
    };

    // One key of a feature that did not resolve.
    struct FeatureMiss
    {
        std::string key;     // the name the plugin gave it
        std::string detail;  // as Failure::detail
    };

    // A declared feature that is off on this build.
    struct FeatureOff
    {
        std::string              id;          // the plugin's id for it
        std::string              title;       // what a player reads
        std::string              reason;      // one line, plain English, from the first miss's Cause
        std::vector<FeatureMiss> misses;      // the keys that did not resolve, in order
        std::vector<std::string> configKeys;  // "Section.Key" or "Section.*" entries it drives

        // True for a feature the server turned off (ApplyHostRules), not one
        // the game build lost. It has no misses, and it is never counted when
        // the loader works out what the build itself switched off.
        bool                     host = false;
    };

    // A key of a declared feature that waits for a game class that is not
    // loaded yet (PLUGIN_FIELD_LATE, a Blueprint class). The feature is off
    // until the plugin's repeated request resolves; this is not a failure and
    // is not in PluginReport, so it never opens the failure window.
    struct PendingKey
    {
        std::string              plugin;
        std::string              key;
        std::string              detail;        // what is waited for, or why the request was refused for good
        std::string              featureId;
        std::string              featureTitle;
        std::vector<std::string> configKeys;    // "Section.Key" or "Section.*" the feature drives
        bool                     refused = false;  // the repeated request was refused: the feature stays off
    };

    // A key of a declared feature that the running game build does not have
    // (VersionMap::SelectField's absent answer): the class or member arrives
    // with a later build. The feature is off, but nothing changed and nothing
    // is missing, so this is information, not a failure: it is not in
    // PluginReport and never opens the failure window.
    struct AbsentKey
    {
        std::string              plugin;
        std::string              key;
        std::string              featureId;
        std::string              featureTitle;
        std::vector<std::string> configKeys;    // "Section.Key" or "Section.*" the feature drives
    };

    // Everything one plugin reported during its last scan session.
    struct PluginReport
    {
        std::string          plugin;    // PluginInfo name, or the DLL file name if unknown
        std::string          file;      // bare DLL file name
        bool                 refused;   // true = the plugin was not loaded (a core failure)
        int                  resolved;  // patterns that DID resolve, for context
        std::vector<Failure> failures;  // core misses: these refuse the plugin
        std::vector<FeatureOff> featuresOff;  // the plugin loaded without these

        // True for a Stage 1 preload plugin (see preload/preload_manager.h)
        // rather than an ordinary Plugins-folder one. Both go through this
        // report because the failure is the same failure and the user should
        // see it in the same place -- but the consequence differs and the text
        // has to say so: a refused preload plugin does not hold up the game,
        // it just does not get its early window.
        bool                 preload = false;
    };

    // --- Session, driven by the plugin manager ---------------------------------

    // Open the scan session for one plugin. self is the identity every
    // IPluginHookScanner call must pass back. Any previous report for this
    // plugin is dropped now, so a reload replaces rather than accumulates.
    // nameShared: another loaded plugin has this one's name (see
    // SessionNameShared).
    void BeginSession(const IPluginSelf* self, const char* pluginName, const char* fileName,
                      bool preload = false, bool nameShared = false);

    // Close the session and commit its failures and its features turned off to
    // the report list (nothing is committed when the plugin resolved everything
    // cleanly). refusedOut, when non-null, receives true if the plugin must not
    // be loaded -- which is any time it recorded a core failure. A feature
    // turned off does not refuse it. A key a declared feature lists that no
    // request in the session asked for is logged as a warning: it can only be a
    // typo, and a typo'd key is one whose miss would refuse the plugin instead.
    void EndSession(const IPluginSelf* self, bool* refusedOut);

    // Record a crash inside OnPluginLoadHooks against the open session. Fatal.
    void RecordSessionCrash(const IPluginSelf* self, const char* detail);

    // --- Recording, called by the IPluginHookScanner wrapper -------------------

    // False (and an error logged) when self has no session open -- which is what
    // stops a plugin from stashing the scanner table and using it later.
    bool HasSession(const IPluginSelf* self);

    // A pattern that resolved. `name` is the key the plugin gave it.
    void RecordResolved(const IPluginSelf* self, const char* name);

    // A field or size row that resolved. Not a pattern, so it is not counted
    // in the report's "resolved", but EndSession knows the key was asked for.
    void RecordFieldResolved(const IPluginSelf* self, const char* key);

    // Why a miss happened, which decides what a feature it turns off says:
    // Game is a locator or a map row that no longer fits this game build;
    // Plugin is a failure the plugin reported itself; Request is a request the
    // loader turned down as malformed, which is a fault in the plugin.
    enum class Cause { Game, Plugin, Request };

    // A miss on hookName. When a feature declared in this session lists the
    // name, every such feature is turned off and the plugin is not refused;
    // otherwise it is a core failure.
    void RecordFailure(const IPluginSelf* self, const char* hookName, const char* detail, bool required,
                       Cause cause = Cause::Game);

    // A PLUGIN_FIELD_LATE request whose class is not loaded yet. Each feature
    // that lists the key goes off for now (*enabled = false, its config entries
    // greyed) and is kept as a PendingKey; the plugin is not refused. A key no
    // feature lists cannot wait: that is a core failure, as RecordFailure.
    void RecordPending(const IPluginSelf* self, const char* key, const char* detail);

    // The repeated request, outside a session. Resolved: the pending entries
    // for the key are dropped (the plugin sets its own enabled flag). Refused:
    // the entries stay, marked, with the reason. Both leave a key that was
    // never pending alone. A resolve that frees a feature also posts the
    // server's rules for that plugin to the next game tick (ReapplyHostRules):
    // the plugin writes its own `enabled` flag only after the call returns.
    void LateResolved(const IPluginSelf* self, const char* key);
    void LateRefused(const IPluginSelf* self, const char* key, const char* detail);

    // Copy of the pending keys of every plugin.
    std::vector<PendingKey> Pending();

    // A request for a key the running build does not have. Each feature that
    // lists the key goes off (*enabled = false, its config entries greyed) and
    // is kept as an AbsentKey; the plugin is not refused. A key no feature
    // lists cannot go off: that is a core failure, as RecordFailure.
    void RecordAbsent(const IPluginSelf* self, const char* key, const char* detail);

    // v72. Declare a feature for the open session. keys are the names later
    // passed to RecordFailure; configKeys are "Section.Key" or "Section.*".
    // *enabled (may be null) is set to true now and to false if a key misses.
    // After the session closes the loader writes it only for the server's list
    // (ApplyHostRules) and a late resolve (LateResolved). False, with nothing declared,
    // for no session, an empty id, title or key list, or an id already declared.
    bool DeclareFeature(const IPluginSelf* self, const char* id, const char* title,
                        const std::vector<std::string>& keys, const std::vector<std::string>& configKeys,
                        bool* enabled);

    // True once a core failure has been recorded in this session (a miss on a
    // key no feature lists). What IPluginHookScanner's HasFailures() answers,
    // so a plugin can stop resolving the moment it knows it is not going to
    // load.
    bool SessionHasFailures(const IPluginSelf* self);

    // v72. Whether the session was opened with nameShared: the map keys a
    // plugin's own sites by its name, so neither of two plugins with one name
    // may read them (VersionMap::ResolveMapKey).
    bool SessionNameShared(const IPluginSelf* self);

    // --- Feature catalog -------------------------------------------------------

    // Every feature a loaded plugin declared, kept from EndSession until the
    // plugin is unloaded or reloaded. The catalog is what a server's rules are
    // applied to: it holds the address of each plugin's `enabled` flag, so an
    // entry must be dropped before its DLL is freed (ForgetFeatures).
    // A plugin that was refused is never in it.
    struct FeatureInfo
    {
        enum class State { On, OffBuild, OffServer };

        std::string plugin;
        std::string id;
        std::string title;
        std::string reason;   // why it is off, empty when it is on
        bool        preload = false;
        State       state   = State::On;
    };

    // Copy of the catalog: every declared feature of every loaded plugin, with
    // whether it is on, off for this game build, or off because of the server.
    std::vector<FeatureInfo> Features();

    // Drop the catalog entries of the plugin `self` and the "turned off by
    // this server" lines they put in the report. Called before every
    // FreeLibrary of that plugin, and by BeginSession for a reload. The
    // server's rules themselves stay: they are the server's, not the
    // plugin's, and apply again when the plugin is back. Never opens the
    // failure window.
    void ForgetFeatures(const IPluginSelf* self);

    // --- Host rules: the server's "this feature stays off" list -----------------

    // Make `deny` (plugin name, feature id) the server's list. Every catalog
    // feature on the list goes off: its `enabled` flag is written false and a
    // FeatureOff "turned off by this server" is added to the plugin's report,
    // which the greyed config page, the plugin list and `hookfailures` already
    // show. Every feature that was on the old list and is not on the new one is
    // handed back (`*enabled = true`) -- but only if the game build has it on.
    // A server can take a feature away, never give one the build lost. The
    // failure window is not opened (no commit serial bump).
    //
    // GAME THREAD ONLY: it writes the plugins' flags. A caller on another
    // thread wraps it in GameThreadDispatch::PostVoid. The list is kept when a
    // plugin unloads or reloads and applied again as the plugin loads.
    void ApplyHostRules(const std::vector<std::pair<std::string, std::string>>& deny);

    // Drop the whole list and its message (ApplyHostRules with nothing). Game
    // thread only. What a joined server sent (ApplyServerRules) is not touched.
    void ClearHostRules();

    // The text that goes with this machine's list: what a server sends to its
    // clients next to the list (`features hostmessage`). Any thread.
    void SetHostMessage(const std::string& message);
    std::string HostMessage();

    // A client joined to a server: the list and message that server sent. Kept
    // apart from the one above, so a received set replaces only what the server
    // set and `features hostoff` entered on this machine stays. The features
    // denied are the two lists together. ClearServerRules is what leaving the
    // server calls. Game thread only (it writes the plugins' flags).
    void ApplyServerRules(const std::vector<std::pair<std::string, std::string>>& deny,
                          const std::string& message);
    void ClearServerRules();

    // Copy of what the joined server sent, and its message. Any thread.
    std::vector<std::pair<std::string, std::string>> ServerDeny();
    std::string ServerMessage();

    // Apply the list again to one plugin's features (by name). What a plugin
    // that writes its `enabled` flag after a late resolve gets, one tick
    // later. Game thread only; a no-op for a plugin that has no denied feature.
    void ReapplyHostRules(const char* plugin);

    // Copy of this machine's own list (what `features hostoff` set, the one a
    // server sends on) as (plugin name, feature id). Not the joined server's.
    std::vector<std::pair<std::string, std::string>> HostDeny();

    // --- Report, for the UI and the console -----------------------------------

    // Copy of every plugin report recorded this session.
    std::vector<PluginReport> Snapshot();

    // Number of plugins with any recorded problem, and the subset that were
    // refused. Cheap enough to call per UI frame.
    int GetReportedPluginCount();
    int GetRefusedPluginCount();

    // Features turned off in one plugin (by name, as PluginInfo gives it). A
    // Stage 1 preload plugin and a Plugins-folder one can share a name; `preload`
    // picks which of the two records is counted. Takes the lock, so cache by
    // GetGeneration() if calling it every frame.
    int GetFeaturesOffCount(const char* plugin, bool preload = false);

    // Why a config entry of `plugin` is switched off: "<feature title> is off:
    // <reason>" when a feature that is off drives "section.key" (or the whole
    // "section.*"), else empty. Names compare without regard to case, as the
    // ini does. Takes the lock and copies, so cache by GetGeneration() if
    // calling it for every row of every frame.
    std::string ConfigOffReason(const char* plugin, const char* section, const char* key);

    // The whole report as plain text: what the clipboard button copies, what the
    // console prints, and what a user can paste to a plugin author.
    std::string BuildReportText();

    // Bumped every time the report list changes -- a session committing
    // failures, or a reload dropping a plugin's old entry. UI that caches
    // anything derived from Snapshot() stores this next to the cache and
    // rebuilds when it moves, rather than copying every string each frame.
    unsigned GetGeneration();

    // Bumped ONLY when a report is added. Separate from the generation because
    // "there is something new to tell the user" and "my cache is stale" are
    // different questions: a reload that now resolves cleanly moves the
    // generation (it drops the old entry) and must not re-open the failure
    // popup, while a plugin hot-loaded into a running game that misses a hook
    // must.
    unsigned GetCommitSerial();

    // Add a report that no scan session produced: the loader's own features
    // the layout check switched off (plugin kLoaderReport), and the F2 menu's
    // test button in debug builds. Opens the failure window like any other.
    // The loader's report is the one whose plugin is kLoaderReport.
    inline constexpr const char* kLoaderReport = "ModLoader";
    void AddReport(const PluginReport& report);
}
