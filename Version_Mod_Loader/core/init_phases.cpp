#include "init_phases.h"
#include "hook_management.h"
#include "engine_sync.h"
#include "../logging/logger.h"
#include "../config/config_manager.h"
#include "../auto_update/auto_updater.h"
#include "../plugins/plugin_manager.h"
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
    ModLoaderLogger::InitializeLogger();
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
    // Engine init has completed and the main thread is held before we get here,
    // so it is always safe to call PluginInit now -- no need to wait for GameInstanceInit.
    ModLoaderLogger::LogInfo(L"[init] Calling PluginInit on all loaded plugins");
    PluginManager::InitAllLoadedPlugins();
    PluginManager::MarkStartupComplete();
}
