#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "plugins/plugin_interface.h"

namespace PluginManager
{
    // Snapshot of a single plugin's state, safe to read after FreeLibrary.
    struct PluginStatus
    {
        char name[64];
        char version[32];
        char author[64];
        bool isLoaded;
        bool isOutOfDate;
        bool needsModLoaderUpdate;
        bool isWrongTarget;

        // True when the plugin's OnPluginLoadHooks event reported a required
        // pattern it could not resolve, so the loader refused it: PluginInit was
        // never called and the DLL was freed. The failure detail itself lives in
        // PluginHookReport (plugins/plugin_hook_report.h) -- this flag is only
        // what the plugin list needs to explain the state it is showing.
        bool hookScanFailed;

        // Bare DLL file name (no directory), e.g. "ServerUtility.dll". Lets the
        // console address a plugin by file when its PluginInfo name is unknown --
        // an out-of-date or crashed-on-load record has no usable name.
        char fileName[128];

        // True when an auto-update sidecar sits beside the DLL (MyPlugin.dll ->
        // MyPlugin.json). That file is what makes a plugin self-updating: the
        // updater's per-plugin pass only ever looks at plugins that have one --
        // see auto_update/auto_updater.h. Checked on disk at snapshot time, not
        // cached, so dropping a sidecar in shows up without a restart.
        bool hasUpdateManifest;

        // Why UNLOAD, or RELOAD/LOAD, would be refused right now, or empty when
        // it would go ahead: the plugin's own load and unload settings, whether a
        // game world is running, and any busy reason it set (plugins/reload_policy.h).
        // Worked out when the snapshot is taken, so it follows the world.
        // unloadBlock covers a loaded plugin's UNLOAD (and with it RELOAD);
        // reloadBlock covers RELOAD of a loaded plugin and LOAD of an unloaded one.
        char unloadBlock[112];
        char reloadBlock[112];

        // True when one of its callbacks crashed and the loader switched it off
        // (plugins/plugin_guard.h): still mapped and still listed as loaded, but
        // nothing calls into it until it is unloaded or reloaded. crashReason
        // says how, e.g. "0xC0000005 in tick callback".
        bool isCrashed;
        char crashReason[96];
    };

    // Initialize the plugin manager
    void InitializePluginManager();

    // Shutdown the plugin manager
    void ShutdownPluginManager();

    // Load all plugin DLLs from the Plugins directory (no PluginInit called).
    // Call InitAllLoadedPlugins() once the engine is ready to complete init.
    void LoadAllPlugins();

    // Call PluginInit on every loaded-but-not-yet-initialized plugin.
    // Safe to call only after UGameInstance::Init has run (GObjects ready).
    void InitAllLoadedPlugins();

    // Unload all plugins
    void UnloadAllPlugins();

    // Get the number of currently loaded plugins
    int GetLoadedPluginCount();

    // Copy up to maxCount PluginInfo pointers into outInfos.
    // Returns the total number of loaded plugins (may exceed maxCount).
    // Returned pointers remain valid as long as plugins are loaded.
    int GetLoadedPluginInfos(const PluginInfo** outInfos, int maxCount);

    // Copy status snapshots for ALL plugin records (loaded and unloaded) into out.
    // Returns total record count (may exceed maxCount).
    int GetAllPluginStatuses(PluginStatus* out, int maxCount);

    // Returns the stable IPluginSelf* for the named plugin record, or nullptr if
    // no plugin with that name has ever been loaded this session. The pointer is
    // valid for the lifetime of the process: plugin records are only ever freed
    // as a batch in UnloadAllPlugins (full shutdown), never removed individually,
    // so it stays stable across UnloadPlugin/ReloadPlugin cycles too.
    const IPluginSelf* GetSelfForPlugin(const char* pluginName);

    // Unload the plugin at index: calls PluginShutdown + FreeLibrary.
    // The record is kept so it can be reloaded later.
    // Returns false if index is out of range or the plugin is already unloaded.
    //
    // Also returns false, before anything is shut down, when the plugin's
    // settings or busy reason refuse it right now; *refusal (if given) then holds
    // the reason, and it is empty for every other failure.
    bool UnloadPlugin(int index, std::string* refusal = nullptr);

    // Reload the plugin at index: unloads it (if currently loaded) then
    // loads it again from the same file path.
    // Returns false if index is out of range or the DLL fails to load/init.
    //
    // A reload of a loaded plugin needs both its unload and its load setting to
    // allow it, and is refused before anything is shut down if either does not;
    // loading an unloaded one needs its load setting. *refusal is filled as for
    // UnloadPlugin.
    bool ReloadPlugin(int index, std::string* refusal = nullptr);

    // Game thread, every tick (engine_tick.cpp, right after GameThreadDispatch::Drain).
    // Acts on plugins the crash guard switched off (plugins/crash_restart.h): a
    // toast and a log line for each, then a ReloadPlugin a few seconds later
    // when its settings allow one, at most CrashRestart::kMaxRestarts times per
    // plugin per session. One atomic load when nothing has crashed. Does
    // nothing once the game has started to exit.
    void PumpCrashRestarts();

    // Game thread, every tick, right after PumpCrashRestarts. Developer feature,
    // off unless modloader.ini [Developer] RemoteReload=1: about once a second
    // it looks for the request file (plugins/remote_reload.h) and reloads each
    // plugin named in it, then logs "[RemoteReload] <Plugin> reloaded OK v<ver>"
    // or "... reload failed: <reason>". When off it is one flag test.
    void PumpRemoteReload();

    // Index of the plugin record matching nameOrFile, or -1 if there is none.
    // Matches (case-insensitively) the PluginInfo name first, then the DLL file
    // name with and without its ".dll" extension.
    int FindPluginIndex(const char* nameOrFile);

    // Module handle of the loaded plugin matching nameOrFile (same matching as
    // FindPluginIndex), or nullptr if there is no such plugin or it is unloaded.
    HMODULE GetPluginModule(const char* nameOrFile);

    // What ScanForNewPlugins found out about one DLL.
    enum class ScanStatus
    {
        Live,          // loaded and initialized; nothing it registered was missed
        Late,          // loaded, but it registered for one-time events that had already fired
        NeedsRestart,  // a new DLL in ModLoader\Preload, which only runs at game start
        Incompatible,  // plugin interface version outside what this loader supports
        WrongTarget,   // built for the other build target (client vs server)
        HooksFailed,   // one of its patterns does not match this game build
        Retry,         // LoadLibrary failed: locked or still being copied
        NotAPlugin,    // loads, but has no plugin exports
        Failed,        // crashed or returned an error while loading or in PluginInit
        Duplicate,     // a plugin with that name is already in the plugin list
        Refused,       // its load setting does not allow loading it now (MENU in a world, RESTART)
    };

    // One-time events that are replayed to a plugin registering late only while
    // startup is still in progress. A plugin that registers for one of them after
    // PluginManager::IsStartupComplete() has missed the firing that already happened.
    enum MissedEvent : unsigned
    {
        MissedChimeraMain    = 1u << 0,   // IPluginWorldEvents::RegisterOnWorldBeginPlay
        MissedAnyWorld       = 1u << 1,   // IPluginWorldEvents::RegisterOnAnyWorldBeginPlay
        MissedExperienceLoad = 1u << 2,   // IPluginWorldEvents::RegisterOnExperienceLoadComplete
    };

    // Called by those three registrations when they skip the replay because
    // startup is over and the event had already fired. Only the plugin being
    // initialized by ScanForNewPlugins is looked at, so this is a plain note.
    void NoteMissedEvent(MissedEvent event);

    struct ScanResult
    {
        ScanStatus  status;
        std::string name;      // plugin name, or the file name when it never got that far
        std::string fileName;  // bare DLL file name
        std::string action;    // what happened and what to do about it, one sentence or two
    };

    // Short label for a status, e.g. "Loaded", "Loaded late", "Needs restart".
    const char* ScanStatusLabel(ScanStatus status);

    // Rescan ModLoader\Plugins for DLLs that have no record yet, load them and
    // run PluginInit on each. Returns the number of plugins that came up.
    //
    // For plugin DLLs dropped in while the game is running -- ReloadPlugin only
    // ever reloads a path that was already there at startup.
    //
    // A DLL whose plugin name is already in use by another plugin is refused.
    // A DLL that cannot be loaded right now (locked, still being copied) leaves
    // no record, so the next scan tries it again. New DLLs in ModLoader\Preload
    // are not loaded, only reported: they run at game start. A DLL whose load
    // setting does not allow it now (MENU while a game world runs, RESTART) is
    // loaded just far enough to read it, then freed with no record, and reported
    // as Refused; the next scan tries it again.
    //
    // If outResults is given it receives one entry per DLL the scan found: what
    // became of it and what to do next.
    //
    // Game thread only: PluginInit runs here.
    int ScanForNewPlugins(std::vector<ScanResult>* outResults = nullptr);

    // Refuses every plugin DLL load from now on -- startup, ReloadPlugin and
    // ScanForNewPlugins alike. Set by the init thread, before any plugin
    // loads, when the startup layout check finds the game no longer matches
    // the SDK.
    void RefusePluginLoads();

    // Increments every time a plugin is loaded, unloaded or reloaded.
    //
    // For UI that caches anything derived from a plugin: store the value alongside
    // the cache and rebuild when it changes. Cheaper and harder to get wrong than
    // a notification per cache, and it catches reloads of the thing already on
    // screen -- which is the case that was broken, because nothing else about the
    // selection changes when you reload the plugin you are already looking at.
    unsigned GetPluginGeneration();

    // Returns true after the initial startup batch of InitAllLoadedPlugins completes.
    // Used by hooks to gate late-registration replay: only replay during startup, not
    // after runtime hot-reloads.
    bool IsStartupComplete();

    // Called by dllmain after the initial plugin init batch to close the startup window.
    void MarkStartupComplete();

    // Manual-reset event signalled when InitAllLoadedPlugins completes.
    // MainInitThreadProc pumps messages waiting on this before closing the splash.
    HANDLE GetPluginsInitializedEvent();
}
