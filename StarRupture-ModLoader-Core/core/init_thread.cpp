#include "init_thread.h"
#include "globals.h"
#include "version_check.h"
#include "startup_utils.h"
#include "init_phases.h"
#include "hook_management.h"
#include "client_ui.h"
#include "../game_type_checker.h"
#include "../console/server_console.h"
#include "../logging/log.h"
#include "../logging/logger.h"
#include "../auto_update/auto_updater.h"
#include "../UI/splash_window.h"
#include "../hooks/game/world_begin_play/world_begin_play.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../plugins/plugin_watchdog.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "../hooks/input/input_processor.h"
#include "../hooks/game/crash_reporter/crash_dialog.h"
#endif
#include <hooks/input/input_hook.h>
#include "../memory_scanner/pattern_preflight.h"
#include "sdk_globals.h"
#include "layout_check.h"
#include "../memory_scanner/version_map.h"
#include "../memory_scanner/version_feed.h"
#include "../auto_update/feed_fetch.h"
#include "../hooks/game/version_map.generated.h"
#include "compat_report.h"
#include "../plugins/plugin_manager.h"
#include "../plugins/plugin_hook_report.h"
#include "../preload/preload_manager.h"
#include "ui_state.h"
#include "../hooks/game/engine_init/engine_init.h"
#include "../utils/thread_utils.h"
#include "../utils/pak_list.h"

#ifdef MODLOADER_CLIENT_BUILD
// Safe-off after a layout mismatch: the SDK reads the wrong bytes on this game
// build, so no plugin code may run. Called on the init thread with the game
// thread parked in the engine-init hook. That park ends on its own after a
// timeout, so plugin loads are refused and the preload plugins shut down
// before the dialog waits on the player. The loader's hooks stay in; the
// world they read comes from SdkGlobals::GetWorld, which with no per-build
// fields resolved (they are not, after a safe-off) reads GWorld: on a build
// other than the SDK's own the resolved one, not Offsets::GWorld.
static void EnterLayoutSafeOff(const LayoutCheck::Result& layout)
{
    PluginManager::RefusePluginLoads();
    PreloadManager::Shutdown();
    ModLoaderLogger::LogError(L"[init] Layout mismatch -- plugins are switched off for this session");

    const std::wstring reportName = CompatReport::Write(layout, CompatReport::Outcome::SafeOff);
    Splash::Close();

    // CRLF: the crash dialog's edit control does not render bare '\n'.
    std::wstring details = L"Your game build:  " + GetGameVersionString() + L"\r\n";
    if (!reportName.empty())
        details += L"Report:           ModLoader\\" + reportName + L"\r\n";
    details += L"\r\n";
    for (const std::string& line : layout.lines)
        details += std::wstring(line.begin(), line.end()) + L"\r\n";

    const auto choice = Hooks::CrashDialog::Show(Hooks::CrashDialog::Mode::LayoutMismatch, details);
    if (choice == Hooks::CrashDialog::Result::QuitGame)
    {
        ModLoaderLogger::LogInfo(L"[init] User chose to quit the game after the layout mismatch");
        ExitProcess(0);
    }
    ModLoaderLogger::LogInfo(L"[init] User chose to start the game without plugins");

    // Let the game thread go, and tell shutdown there is nothing to wait for.
    if (g_pluginsReadyEvent)
        SetEvent(g_pluginsReadyEvent);
    if (g_pluginsLoadedEvent)
        SetEvent(g_pluginsLoadedEvent);
}

// A layout mismatch that only some readers depend on (LayoutCheck::Decide):
// the loader features that read a changed class switch off, and so does every
// plugin that keeps the game's layouts compiled in. Preload plugins all do
// (they have no way to take this build's layouts), and they are already
// running, so they shut down here. Plugins that take every layout from the
// version map still load; a request of theirs for a changed class's compiled
// offset is refused (VersionMap::SelectField), which turns off only the
// feature that asked for it.
static void SwitchOffChangedLayoutUsers(const LayoutCheck::Result& layout)
{
    const LayoutCheck::Verdict& verdict = layout.verdict;
    PreloadManager::Shutdown();
    CompatReport::Write(layout, CompatReport::Outcome::Partial);

    std::vector<std::string> classNames, paths;
    for (size_t i = 0; i < verdict.changed.size(); ++i)
    {
        classNames.push_back(verdict.changed[i]);
        const std::string& path = verdict.changedPaths[i];
        const size_t dot = path.find('.');
        if (dot != std::string::npos)
        {
            classNames.push_back("/Script/" + path.substr(0, dot) + "." + verdict.changed[i]);
            paths.push_back(path);
        }
    }
    VersionMap::SetLayoutChanges(classNames, paths);

    std::string changed;
    for (const std::string& name : verdict.changed)
        changed += (changed.empty() ? "" : ", ") + name;

    const bool menusOff = std::find(verdict.featuresOff.begin(), verdict.featuresOff.end(),
        LayoutCheck::kConsumerUiMenus) != verdict.featuresOff.end();
    if (menusOff)
    {
        UIState::DisableMenuRead();

        PluginHookReport::PluginReport report;
        report.plugin   = PluginHookReport::kLoaderReport;
        report.file     = "StarRupture-ModLoader-Core.dll";
        report.refused  = false;
        report.resolved = 0;
        report.featuresOff.push_back({ "ui-menus", "Menu state for plugins",
            "this game build changed the layout of " + changed + ", so plugins that react to the map, inventory,"
            " build menu or a station menu opening will not see it open", {}, {} });
        PluginHookReport::AddReport(report);
    }

    ModLoaderLogger::LogWarn(L"[init] Layout changed on this build (%S): %S; plugins that keep the game's layouts"
        L" compiled in are not loaded", changed.c_str(),
        menusOff ? "the menu state for plugins is switched off" : "no loader feature reads them");
}
#endif

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

    // Log the process environment now, while the main thread is held, and
    // never after ReleaseMainThread(). This block calls GlobalMemoryStatusEx,
    // and under Wine/Proton that call must not race the game's own first one:
    // Wine's implementation keeps a one-second cache that it timestamps
    // *before* filling, so a second thread arriving during the process's first
    // call is handed an all-zero struct and TRUE. The game's first heap
    // allocation (FMemory::GCreateMalloc, CRT static init, before WinMain)
    // makes that call and sizes FMallocBinned2 from TotalPhysical; zero fails
    // a checkf whose handler allocates, re-enters the one-time initialiser on
    // the same thread and deadlocks with two threads and an empty log -- issue
    // #126, a coin flip per launch in v1.21.0 where this ran in the same
    // millisecond as the release below. Once any call has completed in the
    // process a later race only returns stale-but-valid numbers, so making
    // ours here, with the game frozen, closes the window. Native Windows has
    // no such cache and never saw it.
    LogStartupEnvironment();

    Splash::Show();
    LogToFile::Info("[init] Splash window shown");

    // Bring the ModLoaderLogger online before installing hooks -- otherwise
    // every ModLoaderLogger::Log* call made during hook installation
    // (LogMessage, LogDebug, LogWarn, etc.) is silently dropped because
    // g_logInitialized is still false.
    ModLoaderLogger::InitializeLogger();

#ifdef MODLOADER_CLIENT_BUILD
    // modloader.ini [Debug] DisablePluginVersionMap=1 (default 0) takes the map away
    // from plugins, so a plugin author can test the plugin's own patterns and rows.
    // Read once; the loader's own lookups keep the map.
    if (GetPrivateProfileIntW(L"Debug", L"DisablePluginVersionMap", 0, GetModLoaderDirPath(L"modloader.ini").c_str()))
    {
        VersionMap::SetPluginMapOff(true);
        ModLoaderLogger::LogWarn(L"[Map] modloader.ini [Debug] DisablePluginVersionMap=1: plugins get no map rows and use their own patterns and rows");
    }

    // The map every lookup reads is fixed here, once, before anything reads
    // it: the one compiled into the loader, plus the signed feed cached by an
    // earlier launch if it verifies. The server stays on its exact build.
    // A new exe the map has never seen is fetched for first, here, with the
    // main thread parked.
    const FeedFetch::Config feedConfig = FeedFetch::LoaderConfig();
    const bool feedAsked = FeedFetch::Stage1(feedConfig, g_mainThreadParked, [](uint32_t cl)
        { Splash::SetStatus((L"Downloading game data for CL-" + std::to_wstring(cl)).c_str()); });
    VersionFeed::ActivateCached(feedConfig.cachePath);
#endif

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
        // The game is booting now, so WinHTTP is safe. Ask for a feed for the
        // next launch before Quit's exit; Stage 3 never runs after a refusal.
        FeedFetch::AfterRefusal(feedConfig, feedAsked);
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
        FeedFetch::AfterRefusal(feedConfig, feedAsked);
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

    SdkGlobals::ResolveAndApply();

    LogToFile::Info("[init] Installing engine hooks...");
    const ULONGLONG hooksStart = GetTickCount64();
    InstallHooksPhase();
    LogToFile::Info("[init] Engine hooks installed in %llu ms", GetTickCount64() - hooksStart);

    // The last thing that happens before the game is allowed to start, and the
    // only point at which a plugin can patch a function the engine calls on its
    // way up. Everything it can go wrong with is contained inside RunPhase():
    // the game boots either way. See preload/preload_manager.h.
    const ULONGLONG preloadStart = GetTickCount64();
    PreloadPhase();
    LogToFile::Info("[init] Preload phase finished in %llu ms", GetTickCount64() - preloadStart);

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
    PluginWatchdog::Start();
    LogToFile::Info("[init] WorldBeginPlay + EngineTick hooks installed");

    WaitForEnginePhase();
    LogToFile::Info("[init] Stage 2 complete in %llu ms -- engine ready", GetTickCount64() - stage2Start);

    // Inventory the game's pak files now that the engine is up -- logged to
    // modloader.log and cached for the crash dialog, where user-installed pak
    // mods are the most common real cause of "modloader" crashes.
    PakList::CaptureAndLog();

#ifdef MODLOADER_CLIENT_BUILD
    // Compare the SDK's class layouts with the game's reflection data. Only
    // once engine init has been seen (FEngineLoop::Init returned, or one of
    // its fallbacks fired): every native class is registered by then, and the
    // game thread is parked in the engine-init hook until the plugins are up,
    // so it is not changing the reflection data. A failed hook install or a
    // timed-out wait leaves the game thread running, so the check is skipped.
    // A mismatch the loader or every plugin depends on, or one nothing says
    // who reads, switches plugins off and ends the boot here; any other turns
    // off only what reads the changed classes (SwitchOffChangedLayoutUsers).
    //
    // On a build other than the SDK's own, plugins run only on what was
    // checked: a check that could not run, or an SDK still on its built-in
    // Offsets:: for GObjects or FName::AppendString, switches them off too.
    // What the check found stays in every report below, also when a later step
    // switches the plugins off after only some features went off.
    LayoutCheck::Result layout;
    {
        bool ran = false;
        if (Hooks::EngineInit::IsEngineInitialized())
        {
            ran = LayoutCheck::Run(layout);
            if (ran && LayoutCheck::FoundChanges(layout))
            {
                if (layout.verdict.safeOff)
                {
                    EnterLayoutSafeOff(layout);
                    return 0;
                }
                SwitchOffChangedLayoutUsers(layout);
            }
        }
        else
        {
            ModLoaderLogger::LogWarn(L"[LayoutCheck] Skipped: engine init was not observed, so the game thread is not parked");
        }

        if (VersionMap::RunningExe().cl != VersionMap::kBuiltCL)
        {
            LayoutCheck::Result unchecked = layout;
            if (!ran)
                unchecked.lines.push_back("The layout check did not run, and this is not the build the SDK was made for");
            if (!SdkGlobals::SdkOnResolvedGlobals())
                unchecked.lines.push_back("GObjects or FName::AppendString was not found on this build");
            if (unchecked.lines.size() > layout.lines.size())
            {
                EnterLayoutSafeOff(unchecked);
                return 0;
            }
        }
    }
#endif

    // Field requests read the game's reflection data from here on, on the
    // dedicated server as on the client.
    VersionMap::SetLiveFieldSource(LayoutCheck::LiveFieldOf);
#ifdef MODLOADER_CLIENT_BUILD
    // The loader's own fields are resolved from it. On a build other than the
    // SDK's own, one that does not resolve switches plugins off.
    {
        const std::vector<std::string> failures = SdkGlobals::ResolveFields();
        if (!failures.empty())
        {
            LayoutCheck::Result unresolved = layout;
            unresolved.lines.insert(unresolved.lines.end(), failures.begin(), failures.end());
            EnterLayoutSafeOff(unresolved);
            return 0;
        }
    }
#else
    SdkGlobals::ResolveFields();
#endif

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

    // Install every plugin-facing event hook that is not up yet. Each one
    // otherwise installs itself lazily on the first RegisterPluginCallback,
    // which makes a plugin subscribing from PluginInit the thread that patches
    // live engine code -- and silently unsubscribed if that patch fails. See
    // InstallPluginEventHooks() in hook_management.cpp for the full reasoning.
    LogToFile::Info("[init] Installing remaining plugin event hooks...");
    const ULONGLONG eventHooksStart = GetTickCount64();
    InstallPluginEventHooks();
    LogToFile::Info("[init] Plugin event hooks installed in %llu ms", GetTickCount64() - eventHooksStart);

    LogToFile::Info("[init] Initialising plugins...");
    const ULONGLONG pluginInitStart = GetTickCount64();
    InitPluginsPhase();
    LogToFile::Info("[init] Plugins initialised in %llu ms", GetTickCount64() - pluginInitStart);
#ifdef MODLOADER_CLIENT_BUILD
    // The sizes the plugins asked for are known now.
    CompatReport::Refresh(layout);
#endif

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
