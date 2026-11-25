#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "plugins/plugin_interface.h"
#include "network_channel/wire_compat.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "plugins/plugin_manage.h"
#include "auto_update/mod_install.h"
#endif

namespace PluginManager
{
    // Snapshot of a single plugin's state, safe to read after FreeLibrary.
    struct PluginStatus
    {
        char name[64];
        char version[32];
        char author[64];
        bool isLoaded;   // initialized, or inside its PluginShutdown for an unload or reload
        bool isOutOfDate;
        bool needsModLoaderUpdate;
        int  declaredInterface;   // the plugin interface it declares; kept for a plugin refused as too new
        bool isWrongTarget;

        // True when the plugin's OnPluginLoadHooks event reported a required
        // pattern it could not resolve, so the loader refused it: PluginInit was
        // never called and the DLL was freed. The failure detail itself lives in
        // PluginHookReport (plugins/plugin_hook_report.h) -- this flag is only
        // what the plugin list needs to explain the state it is showing.
        bool hookScanFailed;

        // On modloader.ini [Plugins] Disabled (client builds): the loader does
        // not load it. With isLoaded still set it is off after a restart.
        bool isDisabled;

        // Uninstalled this session (client builds). With isLoaded still set it is
        // gone after a restart.
        bool isRemoved;

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

    // IPluginRegistry::GetPluginVersion: the version of the running plugin with
    // this name (case-insensitive), copied into out. False for no such plugin, one
    // that is disabled, unloaded or being shut down, a null or empty name, or an
    // out too small for the whole version. Any thread; takes g_pluginLock only.
    bool GetPluginVersion(const char* name, char* out, size_t cap);

    // IPluginRegistry::GetDeclaredFeatures: the declared-features text (see
    // plugins/declared_features_text.h), copied into buf as snprintf does. -1
    // until the startup plugin init has finished. Any thread.
    int GetDeclaredFeatures(char* buf, size_t size);

    // IPluginRegistry::GetFeatureCatalog: the cached signed plugin catalog (see
    // plugins/plugin_catalog.h), copied into buf as snprintf does. -1 when there
    // is none, and always on a server build. Any thread.
    int GetFeatureCatalog(char* buf, size_t size);

    // Copy status snapshots for ALL plugin records (loaded and unloaded) into out.
    // Returns total record count (may exceed maxCount).
    int GetAllPluginStatuses(PluginStatus* out, int maxCount);

    // What to say about a plugin built for plugin interface `declared` that this
    // loader refused as too new: a capitalised sentence, such as
    // "Needs ModLoader v1.28.0 or newer", worked out from the latest release the
    // loader's manifest named (auto_update/loader_manifest.h). Cut to cap like snprintf.
    const char* DescribeLoaderNeed(int declared, char* buf, size_t cap);

    // Returns the stable IPluginSelf* for the named plugin record, or nullptr if
    // no plugin with that name has ever been loaded this session. The pointer is
    // valid for the lifetime of the process: plugin records are only ever freed
    // as a batch in UnloadAllPlugins (full shutdown), never removed individually,
    // so it stays stable across UnloadPlugin/ReloadPlugin cycles too.
    const IPluginSelf* GetSelfForPlugin(const char* pluginName);

    // What the named plugin's GetPluginWireInfo export declared, copied at load,
    // or an undeclared Decl when it has no such export, is not loaded, or no
    // plugin has that name. The network channel asks it which other versions of
    // the plugin its packets are compatible with.
    WireCompat::Decl GetWireDecl(const char* pluginName);

    // Name, version and wire declaration of every initialized plugin, for the
    // wire table the server sends behind its manifest ack.
    std::vector<WireCompat::Entry> GetWireEntries();

    // The plugin interface version the plugin loaded from `module` declared in
    // its PluginInfo, or 0 when no loaded plugin owns that module. Lets a
    // registry decide whether a descriptor the plugin passed has the fields a
    // later interface appended: a plugin built before them hands over a smaller
    // struct that must not be read past its end.
    int GetInterfaceVersionForModule(HMODULE module);

    // Unload the plugin at index: calls PluginShutdown + FreeLibrary.
    // The record is kept so it can be reloaded later.
    // Returns false if index is out of range or the plugin is already unloaded.
    //
    // Also returns false, before anything is shut down, when the plugin's
    // settings or busy reason refuse it right now, or when it is already inside
    // its PluginShutdown for another unload or reload; *refusal (if given) then
    // holds the reason, and it is empty for every other failure.
    //
    // PluginShutdown runs without the plugin manager's lock, so the plugin list
    // can be read, and the render thread can draw it, while a plugin shuts down.
    bool UnloadPlugin(int index, std::string* refusal = nullptr);

    // Reload the plugin at index: unloads it (if currently loaded) then
    // loads it again from the same file path.
    // Returns false if index is out of range or the DLL fails to load/init.
    //
    // A reload of a loaded plugin needs both its unload and its load setting to
    // allow it, and is refused before anything is shut down if either does not;
    // loading an unloaded one needs its load setting. *refusal is filled as for
    // UnloadPlugin, and PluginShutdown runs without the plugin manager's lock as
    // it does there.
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
        Disabled,      // the player turned it off (modloader.ini [Plugins] Disabled)
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

#ifdef MODLOADER_CLIENT_BUILD
    // ---- Disable, enable, uninstall, restore and install (client builds) ---

    enum class ManageOp { Disable, Enable, Uninstall, Restore, Install };

    enum class ManageResult
    {
        None,              // nothing asked for this file yet
        Done,
        AfterRestart,      // saved or moved; the running game changes at the next start
        NotReady,          // the plugins are still starting
        Vortex,            // Vortex manages its files: install or uninstall it there
        AlreadyInstalled,  // restore: a copy is in ModLoader\Plugins
        SaveFailed,        // the disabled list could not be written; nothing changed
        MoveFailed,        // the files could not be moved; nothing changed
        RollbackFailed,    // files are split between Plugins and Removed
        Failed,            // see ModLoader.log
    };

    struct ManagedPlugin
    {
        std::string fileName;            // identity: bare DLL name
        std::string name, version, author; // empty when never reported
        PluginManage::CardFacts facts;   // pass to PluginManage::DeriveCard
        bool vortexUnsure = false;       // a Vortex manifest could not be read
        ManageOp     lastOp = ManageOp::Disable;
        ManageResult lastResult = ManageResult::None;
        std::string  message;            // why, when the result needs saying
        // Install only: the mod list it came from passed its signature check.
        bool         listSigned = false;
    };

    struct ManageSnapshot
    {
        std::vector<ManagedPlugin> plugins;
        std::vector<std::string> preloadNames; // for ModIndex::IsInstalled
        bool installing = false;               // one install runs at a time
    };

    // Whether a mod index entry describes this file: by the DLL it installs, by
    // the name the plugin reports or, when it never reported one, by its file
    // name.
    inline bool ListingMatches(const ModIndex::Entry& entry, const ManagedPlugin& plugin)
    {
        if (!entry.install.dllFile.empty() &&
            ModIndex::detail::EqualsNoCase(plugin.fileName, entry.install.dllFile))
            return true;
        const std::string name = !plugin.name.empty()
            ? plugin.name : plugin.fileName.substr(0, plugin.fileName.rfind('.'));
        return ModIndex::IsInstalled(entry, { name });
    }

    // Any thread, the render thread included. The work runs on the next game
    // tick; until then the file's facts.working is set. A request for a file
    // that is already working is dropped.
    void RequestPluginChange(ManageOp op, const std::string& fileName);

    // Any thread. Copies what is in memory: no file or UObject access.
    ManageSnapshot GetManageSnapshot();

    // Why `entry` cannot be installed now, in plain words; empty: it can. `list`
    // is the whole mod list, for the names of what the entry needs. Any thread.
    // Reads only what is passed in; the installer checks the disk itself.
    std::string InstallBlockedReason(const ModIndex::Entry& entry, const std::vector<ModIndex::Entry>& list,
        const ManageSnapshot& managed);

    // Any thread. Downloads the entry's DLL, checks its size and SHA-256, writes
    // it and loads it on the game thread, or says that it waits for a restart.
    // The card of entry.install.dllFile shows the progress and the result: lastOp
    // is Install, and facts.working is set until it ends. A test passes its own
    // installer.
    void RequestPluginInstall(const ModIndex::Entry& entry, const std::vector<ModIndex::Entry>& list,
        bool listSigned, ModInstall::Installer& installer = ModInstall::Instance());

    // Game thread. Reads the plugin folder, the disabled list, the backups and
    // the Vortex manifests again. The loader does it after startup, a scan and
    // each change.
    void RefreshManagedFiles();
#endif

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
