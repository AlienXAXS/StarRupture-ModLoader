#include "shutdown.h"
#include "globals.h"
#include "hook_management.h"
#include "client_ui.h"
#include "../logging/log.h"
#include "../logging/logger.h"
#include "../config/config_manager.h"
#include "../plugins/plugin_manager.h"
#include "../network_channel/network_channel.h"
#include "../dwmapi_proxy.h"
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

    ModLoaderLogger::LogInfo(L"======================================");
    ModLoaderLogger::LogInfo(L"       Modloader shutting down!");
    ModLoaderLogger::LogInfo(L"======================================");

    ModLoaderLogger::LogInfo(L"Removing engine shutdown hook...");
    Hooks::EngineShutdown::Remove();

    PluginManager::UnloadAllPlugins();
    NetworkChannel::Shutdown();

    RemoveAllHooks();

#ifdef MODLOADER_CLIENT_BUILD
    ShutdownClientUI();
#endif

    PluginManager::ShutdownPluginManager();
    ModLoaderLogger::ShutdownConfigManager();

    ModLoaderLogger::LogInfo(L"Shutting down dwmapi proxy...");
    ModLoaderLogger::LogInfo(L"Goodbye!");
    ModLoaderLogger::ShutdownLogger();

    DwmapiProxy::Shutdown();
    LogToFile::Shutdown();
}
