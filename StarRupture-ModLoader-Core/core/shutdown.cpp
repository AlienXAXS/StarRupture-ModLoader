#include "shutdown.h"
#include "globals.h"
#include "hook_management.h"
#include "client_ui.h"
#include "../logging/log.h"
#include "../logging/logger.h"
#include "../config/config_manager.h"
#include "../plugins/plugin_manager.h"
#include "../network_channel/network_channel.h"
#include "../console/server_console.h"
#include "../hooks/game/engine_shutdown/engine_shutdown.h"

void ShutdownAll()
{
    if (g_autoUpdateThread)
    {
        WaitForSingleObject(g_autoUpdateThread, 15'000);
        CloseHandle(g_autoUpdateThread);
        g_autoUpdateThread = NULL;
    }

    if (g_engineReadyEvent)
    {
        CloseHandle(g_engineReadyEvent);
        g_engineReadyEvent = NULL;
    }

    if (g_ue4ssReadyEvent)
    {
        SetEvent(g_ue4ssReadyEvent);
        CloseHandle(g_ue4ssReadyEvent);
        g_ue4ssReadyEvent = NULL;
    }

    if (g_pluginsLoadedEvent)
    {
        if (WaitForSingleObject(g_pluginsLoadedEvent, 30'000) == WAIT_TIMEOUT)
            LogToFile::Warn("Timed out waiting for init thread -- proceeding with shutdown anyway");
        CloseHandle(g_pluginsLoadedEvent);
        g_pluginsLoadedEvent = NULL;
    }

    if (g_mainInitThread)
    {
        CloseHandle(g_mainInitThread);
        g_mainInitThread = NULL;
    }

    // Owned by the Stage 1 gate (MainInitApcProc / MainInitThreadProc); both
    // are long finished by the time we get here.
    if (g_stage1DoneEvent)
    {
        CloseHandle(g_stage1DoneEvent);
        g_stage1DoneEvent = NULL;
    }

    ModLoaderLogger::LogInfo(L"======================================");
    ModLoaderLogger::LogInfo(L"       Modloader shutting down!");
    ModLoaderLogger::LogInfo(L"======================================");

    ModLoaderLogger::LogInfo(L"Removing engine shutdown hook...");
    Hooks::EngineShutdown::Remove();

    // Before the plugins go: a console command dispatched from here on would be
    // operating on a plugin list that is being torn down underneath it.
    ServerConsole::Shutdown();

    PluginManager::UnloadAllPlugins();
    NetworkChannel::Shutdown();

    RemoveAllHooks();

#ifdef MODLOADER_CLIENT_BUILD
    ShutdownClientUI();
#endif

    PluginManager::ShutdownPluginManager();
    ModLoaderLogger::ShutdownConfigManager();

    ModLoaderLogger::LogInfo(L"Goodbye!");
    ModLoaderLogger::ShutdownLogger();

    // LogToFile::Shutdown() and DwmapiProxy::Shutdown() are owned by the proxy
    // (StarRupture-ModLoader-Proxy\dllmain.cpp) -- this DLL never opened the real
    // dwmapi.dll handle and closing the shared log file here would race with
    // the proxy's own use of it after Core_Detach() returns.
}
