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
#ifdef MODLOADER_CLIENT_BUILD
#include "../hooks/input/input_processor.h"
#include "../hooks/game/crash_reporter/crash_dialog.h"
#endif
#include <hooks/input/input_hook.h>
#include "../memory_scanner/pattern_preflight.h"
#include "../utils/thread_utils.h"

DWORD WINAPI MainInitThreadProc(LPVOID)
{
    // ------------------------------------------------------------------
    // Stage 1: Hook installation
    // Install all detours before the engine gets far enough to miss them.
    // The main thread runs freely here -- hook patching requires it.
    // ------------------------------------------------------------------
    {
        const auto typeResult = GameTypeChecker::Check();
        if (typeResult == GameTypeChecker::Result::SilentBail)
            return 0;
        if (typeResult == GameTypeChecker::Result::ErrorAndExit)
            ExitProcess(1);
    }

    Splash::Show();

    // Bring the ModLoaderLogger online before installing hooks -- otherwise
    // every ModLoaderLogger::Log* call made during hook installation
    // (LogMessage, LogDebug, LogWarn, etc.) is silently dropped because
    // g_logInitialized is still false.
    ModLoaderLogger::InitializeLogger();

    // Version check before hook installation -- if the game build is wrong
    // we must not install hooks (the engine init hook would block the main
    // thread waiting on g_pluginsReadyEvent which would never be signalled).
    if (!VerifyGameVersion())
        return 0;

    // Preflight every modloader scan pattern before installing anything.
    // If even one required pattern is missing (e.g. after a game update),
    // abort completely -- the game must never run partially hooked. The
    // successful case doubles as a scan-cache warm-up, so the per-hook
    // scans below become cache hits.
    Splash::SetStatus(L"Verifying scan patterns...");
    std::wstring preflightDetails;
    if (!PatternPreflight::VerifyAllPatterns(&preflightDetails))
    {
        ModLoaderLogger::LogError(L"[init] Pattern preflight failed -- mod loader disabled");
        Splash::Close();

#ifdef MODLOADER_CLIENT_BUILD
        // Pause the game while the dialog is up -- no hooks are installed at
        // this point, so nothing else stops the game from booting behind the
        // dialog. suspend_main_thread() safely returns NULL if we *are* the
        // main thread (APC init path), where the modal dialog itself already
        // blocks the game.
        HANDLE suspendedMainThread = suspend_main_thread();

        // Let the user decide: continue with a completely unmodified game, or
        // quit and wait for a ModLoader update.
        const auto choice = Hooks::CrashDialog::Show(
            Hooks::CrashDialog::Mode::PatternFailure, preflightDetails);

        resume_main_thread(suspendedMainThread);
        if (choice == Hooks::CrashDialog::Result::QuitGame)
        {
            ModLoaderLogger::LogInfo(L"[init] User chose to quit the game after preflight failure");
            ExitProcess(0);
        }
        ModLoaderLogger::LogInfo(L"[init] User chose to start the game without the mod loader");
#else
        // Dedicated server: no UI to ask -- log and continue unmodified.
        ModLoaderLogger::LogError(L"[init] Starting the game unmodified");
#endif
        return 0;
    }

    InstallHooksPhase();
    LogToFile::Info("[init] Stage 1 complete -- hooks installed, main thread running");

    // ------------------------------------------------------------------
    // Stage 2: Subsystem init + wait for engine ready
    // Hooks are in place; now initialise subsystems and block until the
    // engine fires the ready signal through one of those hooks.
    // ------------------------------------------------------------------
    Splash::SetStatus(L"Starting mod loader...");
    Splash::SetProgress(0.0f);

    LogStartupEnvironment();
    InitSubsystems();

#ifdef MODLOADER_CLIENT_BUILD
    ModLoaderLogger::LogInfo(L"Running client build of modloader");
    Hooks::Input::InstallInputProcessor();
    Hooks::InputHook::Install();
#else
    ModLoaderLogger::LogInfo(L"Running server build of modloader");
#endif

    Hooks::WorldBeginPlay::Install();
    Hooks::EngineTick::Install();

    WaitForEnginePhase();
    ModLoaderLogger::LogInfo(L"[init] Stage 2 complete -- engine ready");

    // ------------------------------------------------------------------
    // Stage 3: Plugin loading and initialisation
    // Run the auto-updater first so plugin DLLs on disk are up to date
    // before we load them. The main thread is held by the engine init hook
    // for the duration of this stage.
    // ------------------------------------------------------------------
#ifdef MODLOADER_CLIENT_BUILD
    InitClientUI();
#endif

    Splash::SetStatus(L"Checking for plugin updates...");
    g_autoUpdateThread = CreateThread(nullptr, 0, AutoUpdateThreadProc, nullptr, 0, nullptr);
    if (g_autoUpdateThread)
    {
        Splash::SetStatus(L"Waiting for update check to complete...");
        WaitForSingleObject(g_autoUpdateThread, INFINITE);
        CloseHandle(g_autoUpdateThread);
        g_autoUpdateThread = NULL;
    }
    else
    {
        LogToFile::Warn("[ModLoader] Failed to create auto-update thread (%lu) -- running synchronously", GetLastError());
        ModLoaderLogger::RunAutoUpdate();
    }

    LoadPluginsPhase();
    InitPluginsPhase();

    ModLoaderLogger::LogInfo(L"[init] Stage 3 complete -- plugins initialised, releasing main thread");
    if (g_pluginsReadyEvent)
        SetEvent(g_pluginsReadyEvent);

    Splash::SetStatus(L"Ready.");
    Splash::SetProgress(1.0f);
    ModLoaderLogger::LogInfo(L"Mod loader initialised successfully.");

    if (g_pluginsLoadedEvent)
        SetEvent(g_pluginsLoadedEvent);

    // Wait for any plugins that acquired a splash hold during PluginInit
    // (e.g. to show progress for a PostToGameThread callback).
    // 30-second timeout guards against a plugin that forgets to release.
    if (Splash::HasHolds())
        Splash::SetStatus(L"Waiting For Plugins To Finish Loading...");
    Splash::WaitForAllHolds(30'000);
    Splash::Linger(400);
    Splash::Close();

    return 0;
}
