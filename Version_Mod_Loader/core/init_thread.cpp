#include "init_thread.h"
#include "globals.h"
#include "version_check.h"
#include "startup_utils.h"
#include "init_phases.h"
#include "client_ui.h"
#include "../game_type_checker.h"
#include "../logging/log.h"
#include "../logging/logger.h"
#include "../auto_update/auto_updater.h"
#include "../UI/splash_window.h"
#include "../hooks/game/world_begin_play/world_begin_play.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../plugins/plugin_manager.h"
#include "../utils/thread_utils.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "../hooks/input/input_processor.h"
#endif

DWORD WINAPI MainInitThreadProc(LPVOID)
{
    {
        const auto typeResult = GameTypeChecker::Check();
        if (typeResult == GameTypeChecker::Result::SilentBail)
            return 0;
        if (typeResult == GameTypeChecker::Result::ErrorAndExit)
            ExitProcess(1);
    }

#ifdef MODLOADER_CLIENT_BUILD
    ModLoaderLogger::LogInfo(L"Running client build of modloader");
#else
    ModLoaderLogger::LogInfo(L"Running server build of modloader");
#endif

    Splash::Show();
    Splash::SetStatus(L"Starting mod loader...");
    Splash::SetProgress(0.0f);

    if (!VerifyGameVersion())
        return 0;

    LogStartupEnvironment();
    InitSubsystems();

#ifdef MODLOADER_CLIENT_BUILD
    Hooks::Input::InstallInputProcessor();
#endif

    Hooks::WorldBeginPlay::Install();
    Hooks::EngineTick::Install();

    Splash::SetStatus(L"Checking for updates and installing hooks...");
    g_autoUpdateThread = CreateThread(nullptr, 0, AutoUpdateThreadProc, nullptr, 0, nullptr);
    if (!g_autoUpdateThread)
        LogToFile::Warn("[ModLoader] Failed to create auto-update thread (%lu) -- running synchronously", GetLastError());

    if (g_autoUpdateThread)
    {
        InstallHooksPhase();
        WaitForEnginePhase();

        Splash::SetStatus(L"Waiting for update check to complete...");
        WaitForSingleObject(g_autoUpdateThread, INFINITE);
        CloseHandle(g_autoUpdateThread);
        g_autoUpdateThread = NULL;
    }
    else
    {
        Splash::SetStatus(L"Checking for plugin updates...");
        Splash::SetProgress(0.10f);
        ModLoaderLogger::RunAutoUpdate();
        InstallHooksPhase();
        WaitForEnginePhase();
    }

#ifdef MODLOADER_CLIENT_BUILD
    InitClientUI();
#endif

    LoadPluginsPhase();
    InitPluginsIfReady();

    {
        HANDLE hEvent = PluginManager::GetPluginsInitializedEvent();
        if (hEvent && WaitForSingleObject(hEvent, 0) != WAIT_OBJECT_0)
        {
            Splash::SetStatus(L"Waiting for game engine...");
            while (WaitForSingleObject(hEvent, 0) != WAIT_OBJECT_0)
            {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                MsgWaitForMultipleObjects(1, &hEvent, FALSE, 50, QS_ALLINPUT);
            }
        }
    }

    Splash::SetStatus(L"Ready.");
    Splash::SetProgress(1.0f);
    ModLoaderLogger::LogInfo(L"Mod loader injection complete - Yay!");

    if (g_pluginsLoadedEvent)
        SetEvent(g_pluginsLoadedEvent);

    Splash::Linger(1200);
    Splash::Close();

    return 0;
}
