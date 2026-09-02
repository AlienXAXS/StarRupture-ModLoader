#pragma once

#include <windows.h>
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
    bool UnloadPlugin(int index);

    // Reload the plugin at index: unloads it (if currently loaded) then
    // loads it again from the same file path.
    // Returns false if index is out of range or the DLL fails to load/init.
    bool ReloadPlugin(int index);

    // Index of the plugin record matching nameOrFile, or -1 if there is none.
    // Matches (case-insensitively) the PluginInfo name first, then the DLL file
    // name with and without its ".dll" extension.
    int FindPluginIndex(const char* nameOrFile);

    // Rescan ModLoader\Plugins for DLLs that have no record yet, load them and
    // run PluginInit on each. Returns the number of plugins that came up.
    //
    // For plugin DLLs dropped in while the game is running -- ReloadPlugin only
    // ever reloads a path that was already there at startup.
    int ScanForNewPlugins();

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
