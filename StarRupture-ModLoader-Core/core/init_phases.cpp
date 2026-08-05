#include "init_phases.h"
#include "hook_management.h"
#include "engine_sync.h"
#include "../logging/logger.h"
#include "../config/config_manager.h"
#include "../auto_update/auto_updater.h"
#include "../plugins/plugin_manager.h"
#include "../network_channel/network_channel.h"
#include "../UI/splash_window.h"
#include "../hooks/game/game_instance_init/game_instance_init.h"

DWORD WINAPI AutoUpdateThreadProc(LPVOID)
{
    ModLoaderLogger::RunAutoUpdate();
    return 0;
}

void InitSubsystems()
{
    Splash::SetStatus(L"Initializing logger...");
    Splash::SetProgress(0.05f);
    // Logger is brought online earlier in MainInitThreadProc (before
    // InstallHooksPhase) so hook-installation logging isn't dropped.
    ModLoaderLogger::LogMessage(L"======================================");
    ModLoaderLogger::LogMessage(L"  AlienX's Mod Loader Starting");
    ModLoaderLogger::LogMessage(L"======================================");

    ModLoaderLogger::InitializeConfigManager();
    PluginManager::InitializePluginManager();
}

void InstallHooksPhase()
{
    Splash::SetStatus(L"Installing core game hooks...");
    Splash::SetProgress(0.05f);
    InstallAllHooks();
}

void WaitForEnginePhase()
{
    WaitForEngineReady();
    Splash::SetProgress(0.50f);
}

void LoadPluginsPhase()
{
    Splash::SetStatus(L"Loading plugin DLLs...");
    Splash::SetProgress(0.50f);
    PluginManager::LoadAllPlugins();
    Splash::SetProgress(0.85f);
}

void InitPluginsPhase()
{
    // Bring plugin networking up BEFORE any PluginInit, and unconditionally.
    //
    // It used to start lazily, on the first GetPluginHooks() call -- which only
    // happens while initialising a plugin. A loader with no plugins therefore
    // never started it at all, and that stopped being acceptable when the
    // authority acquired a duty it owes regardless of its own plugins: greeting
    // each joining client so the client knows it may speak on the control
    // channel. A pluginless server that never greets makes every client report
    // "this server is not running the mod loader", which is both wrong and
    // exactly the wrong thing to send someone debugging. The same applies to a
    // plugin hot-loaded later: players who joined before it would stay dark
    // until they reconnected.
    //
    // Ordering also improves. A plugin's PluginInit can now register a
    // client-ready callback and have it work, instead of depending on the
    // subsystem having been started as a side effect of resolving its own
    // hooks pointer moments earlier.
    NetworkChannel::Initialize();

    // Engine init has completed and the main thread is held before we get here,
    // so it is always safe to call PluginInit now -- no need to wait for GameInstanceInit.
    ModLoaderLogger::LogInfo(L"[init] Calling PluginInit on all loaded plugins");
    PluginManager::InitAllLoadedPlugins();
    PluginManager::MarkStartupComplete();
}
