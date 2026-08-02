#include "init_thread.h"
#include "globals.h"
#include "version_check.h"
#include "startup_utils.h"
#include "init_phases.h"
#include "client_ui.h"
#include "../game_type_checker.h"
#include "../console/server_console.h"
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
#include "../utils/pak_list.h"

// Lets the game main thread run again at the end of Stage 1. Two ways it can
// be stopped, decided in Core_Attach:
//
//  * APC path (the normal dwmapi proxy load): the main thread parks itself
//    inside MainInitApcProc on g_stage1DoneEvent, at a point where it provably
//    holds no loader or heap lock. Nothing was suspended, so we just signal.
//  * Injected path (Core loaded on some other thread): the game is already
//    running and the main thread has to be suspended from the outside.
//
// Never SuspendThread() the main thread on the APC path: it freezes wherever
// the game's entry point has got to, which is routinely inside LdrLoadDll or
// RtlAllocateHeap, and everything below (splash window creation, the pattern
// preflight's allocations) then blocks forever on the lock it owns.
static void ReleaseMainThread(HANDLE& suspendedMainThread)
{
    if (g_mainThreadParked)
    {
        if (g_stage1DoneEvent)
            SetEvent(g_stage1DoneEvent);
        return;
    }

    resume_main_thread(suspendedMainThread);
    suspendedMainThread = NULL;
}

DWORD WINAPI MainInitThreadProc(LPVOID)
{
    // ------------------------------------------------------------------
    // Stage 1: Hook installation
    // Install all detours before the engine gets far enough to miss them.
    // The game's main thread is held for the whole of Stage 1 -- the game
    // must not keep booting while we scan patterns and patch code. See
    // ReleaseMainThread() above for how it is held.
    // ------------------------------------------------------------------
    // On the APC path the main thread is already parked (nothing to suspend);
    // declared up front so the wrong-build bail below can release it too.
    HANDLE suspendedMainThread = NULL;

    const ULONGLONG stage1Start = GetTickCount64();
    LogToFile::Info("[init] ---- Stage 1: hook installation (init thread %lu) ----", GetCurrentThreadId());

    {
        const auto typeResult = GameTypeChecker::Check();
        if (typeResult == GameTypeChecker::Result::SilentBail)
        {
            LogToFile::Info("[init] Executable is not the expected target for this build -- bailing out silently");
            ReleaseMainThread(suspendedMainThread);
            return 0;
        }
        if (typeResult == GameTypeChecker::Result::ErrorAndExit)
            ExitProcess(1);
    }
    LogToFile::Info("[init] Executable check passed");

    if (g_mainThreadParked)
    {
        LogToFile::Info("[init] Main thread held: parked on the Stage 1 gate");
    }
    else
    {
        suspendedMainThread = suspend_main_thread();
        LogToFile::Info("[init] Main thread held: suspended (handle %s)",
            suspendedMainThread ? "acquired" : "NOT acquired -- game keeps booting during Stage 1");
    }

    Splash::Show();
    LogToFile::Info("[init] Splash window shown");

    // Bring the ModLoaderLogger online before installing hooks -- otherwise
    // every ModLoaderLogger::Log* call made during hook installation
    // (LogMessage, LogDebug, LogWarn, etc.) is silently dropped because
    // g_logInitialized is still false.
    ModLoaderLogger::InitializeLogger();

    // Version check before hook installation -- if the game build is wrong
    // we must not install hooks (the engine init hook would block the main
    // thread waiting on g_pluginsReadyEvent which would never be signalled).
    std::wstring versionDetails;
    if (!VerifyGameVersion(&versionDetails))
    {
        Splash::Close();

#ifdef MODLOADER_CLIENT_BUILD
        // Same deal as the preflight failure below: the main thread is still
        // held, so the game stays frozen while the user decides. The splash has
        // nowhere near enough room to explain why a version mismatch disables
        // the loader by design, so use the full dialog.
        const auto choice = Hooks::CrashDialog::Show(
            Hooks::CrashDialog::Mode::VersionMismatch, versionDetails);

        ReleaseMainThread(suspendedMainThread);
        if (choice == Hooks::CrashDialog::Result::QuitGame)
        {
            ModLoaderLogger::LogInfo(L"[init] User chose to quit the game after version mismatch");
            ExitProcess(0);
        }
        ModLoaderLogger::LogInfo(L"[init] User chose to start the game without the mod loader");
#else
        // Dedicated server: no UI to ask -- log, resume the game unmodified.
        ModLoaderLogger::LogError(L"[init] Starting the game unmodified");
        ReleaseMainThread(suspendedMainThread);
#endif
        return 0;
    }

    // Preflight every modloader scan pattern before installing anything.
    // If even one required pattern is missing (e.g. after a game update),
    // abort completely -- the game must never run partially hooked. The
    // successful case doubles as a scan-cache warm-up, so the per-hook
    // scans below become cache hits.
    Splash::SetStatus(L"Verifying scan patterns...");
    LogToFile::Info("[init] Pattern preflight starting...");

    const ULONGLONG preflightStart = GetTickCount64();
    std::wstring preflightDetails;
    const bool preflightOk = PatternPreflight::VerifyAllPatterns(&preflightDetails);
    LogToFile::Info("[init] Pattern preflight %s in %llu ms",
        preflightOk ? "passed" : "FAILED", GetTickCount64() - preflightStart);

    if (!preflightOk)
    {
        ModLoaderLogger::LogError(L"[init] Pattern preflight failed -- mod loader disabled");
        Splash::Close();

#ifdef MODLOADER_CLIENT_BUILD
        // The main thread is still held from the top of Stage 1, so the game
        // stays frozen while the dialog is up. Let the user decide: continue
        // with a completely unmodified game, or quit and wait for a ModLoader
        // update.
        const auto choice = Hooks::CrashDialog::Show(
            Hooks::CrashDialog::Mode::PatternFailure, preflightDetails);

        ReleaseMainThread(suspendedMainThread);
        if (choice == Hooks::CrashDialog::Result::QuitGame)
        {
            ModLoaderLogger::LogInfo(L"[init] User chose to quit the game after preflight failure");
            ExitProcess(0);
        }
        ModLoaderLogger::LogInfo(L"[init] User chose to start the game without the mod loader");
#else
        // Dedicated server: no UI to ask -- log, resume the game unmodified.
        ModLoaderLogger::LogError(L"[init] Starting the game unmodified");
        ReleaseMainThread(suspendedMainThread);
#endif
        return 0;
    }

    LogToFile::Info("[init] Installing engine hooks...");
    const ULONGLONG hooksStart = GetTickCount64();
    InstallHooksPhase();
    LogToFile::Info("[init] Engine hooks installed in %llu ms", GetTickCount64() - hooksStart);

    // Hooks are in place -- from here the engine-init hook takes over pausing
    // the main thread (it blocks on g_pluginsReadyEvent), so let it run again.
    ReleaseMainThread(suspendedMainThread);
    LogToFile::Info("[init] Stage 1 complete in %llu ms -- hooks installed, main thread running",
        GetTickCount64() - stage1Start);

    // ------------------------------------------------------------------
    // Stage 2: Subsystem init + wait for engine ready
    // Hooks are in place; now initialise subsystems and block until the
    // engine fires the ready signal through one of those hooks.
    // ------------------------------------------------------------------
    const ULONGLONG stage2Start = GetTickCount64();
    LogToFile::Info("[init] ---- Stage 2: subsystems + wait for engine ----");

    Splash::SetStatus(L"Starting mod loader...");
    Splash::SetProgress(0.0f);

    LogStartupEnvironment();

    LogToFile::Info("[init] Initialising subsystems (config, plugin manager)...");
    InitSubsystems();
    LogToFile::Info("[init] Subsystems initialised");

    // Started here rather than after the plugins so it is up while the engine is
    // still booting -- on a dedicated server that is exactly the stretch where
    // nothing else says anything. Commands that need the game thread simply
    // queue until the first tick. No-op without -console.
    ServerConsole::Start();

#ifdef MODLOADER_CLIENT_BUILD
    ModLoaderLogger::LogInfo(L"Running client build of modloader");
    Hooks::Input::InstallInputProcessor();
    Hooks::InputHook::Install();
    LogToFile::Info("[init] Input hooks installed");
#else
    ModLoaderLogger::LogInfo(L"Running server build of modloader");
#endif

    Hooks::WorldBeginPlay::Install();
    Hooks::EngineTick::Install();
    LogToFile::Info("[init] WorldBeginPlay + EngineTick hooks installed");

    WaitForEnginePhase();
    LogToFile::Info("[init] Stage 2 complete in %llu ms -- engine ready", GetTickCount64() - stage2Start);

    // Inventory the game's pak files now that the engine is up -- logged to
    // modloader.log and cached for the crash dialog, where user-installed pak
    // mods are the most common real cause of "modloader" crashes.
    PakList::CaptureAndLog();

    // ------------------------------------------------------------------
    // Stage 3: Plugin loading and initialisation
    // Run the auto-updater first so plugin DLLs on disk are up to date
    // before we load them. The main thread is held by the engine init hook
    // for the duration of this stage.
    // ------------------------------------------------------------------
    const ULONGLONG stage3Start = GetTickCount64();
    LogToFile::Info("[init] ---- Stage 3: auto-update + plugins ----");

#ifdef MODLOADER_CLIENT_BUILD
    InitClientUI();
    LogToFile::Info("[init] Client UI initialised");
#endif

    Splash::SetStatus(L"Checking for plugin updates...");
    LogToFile::Info("[init] Running plugin auto-update check...");
    const ULONGLONG updateStart = GetTickCount64();

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
    LogToFile::Info("[init] Auto-update check finished in %llu ms", GetTickCount64() - updateStart);

    LogToFile::Info("[init] Loading plugin DLLs...");
    const ULONGLONG loadStart = GetTickCount64();
    LoadPluginsPhase();
    LogToFile::Info("[init] Plugin DLLs loaded in %llu ms", GetTickCount64() - loadStart);

    LogToFile::Info("[init] Initialising plugins...");
    const ULONGLONG pluginInitStart = GetTickCount64();
    InitPluginsPhase();
    LogToFile::Info("[init] Plugins initialised in %llu ms", GetTickCount64() - pluginInitStart);

    LogToFile::Info("[init] Stage 3 complete in %llu ms -- releasing main thread", GetTickCount64() - stage3Start);
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
    {
        LogToFile::Info("[init] Waiting for plugin splash holds to be released (30 s timeout)...");
        Splash::SetStatus(L"Waiting For Plugins To Finish Loading...");
    }
    Splash::WaitForAllHolds(30'000);
    Splash::Linger(400);
    Splash::Close();

    LogToFile::Info("[init] Boot sequence finished in %llu ms -- init thread exiting",
        GetTickCount64() - stage1Start);
    return 0;
}
