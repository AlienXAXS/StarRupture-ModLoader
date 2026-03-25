// dllmain.cpp : Defines the entry point for the DLL application.
#include "logging/log.h"
#include "logging/ue_log.h"
#include "logging/logger.h"

#include "dwmapi_proxy.h"

#include "config/config_manager.h"

#include "plugins/plugin_manager.h"

#include "memory_scanner/scanner.h"

#include "UI/splash_window.h"

#include "hooks/game/world_begin_play/world_begin_play.h"
#include "hooks/game/engine_init/engine_init.h"
#include "hooks/game/engine_shutdown/engine_shutdown.h"
#include "hooks/game/save_loaded/save_loaded.h"
#include "hooks/game/experience_load_complete/experience_load_complete.h"
#include "hooks/game/actor_begin_play/actor_begin_play.h"
#include "hooks/game/player_joined/player_joined.h"
#include "hooks/game/player_left/player_left.h"
#include "hooks/game/mass_spawner_activate/mass_spawner_activate.h"
#include "hooks/game/mass_spawner_deactivate/mass_spawner_deactivate.h"
#include "hooks/game/mass_do_spawning/mass_do_spawning.h"

#include "auto_update/auto_updater.h"

#include "utils/thread_utils.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/input/input_processor.h"
#include "hooks/game/engine_tick/engine_tick.h"
#include "UI/imgui_backend.h"
#include "UI/overlay.h"
#include "UI/global_settings.h"
#include "Engine_classes.hpp"
#endif

#include "DbgHelp.h"
#pragma comment(lib, "DbgHelp.lib")

#include <Psapi.h>
#include <VersionHelpers.h>
#include <winver.h>
#include <thread>
#include <chrono>
#include <vector>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

// ---------------------------------------------------------------------------
// Globals shared between DllMain and the main init thread
// ---------------------------------------------------------------------------

// Handle to the main initialisation thread (kept so DLL_PROCESS_DETACH can
// wait for it in the FreeLibrary case before tearing down subsystems).
static HANDLE g_mainInitThread = NULL;

// Signalled (manual-reset) by the init thread once all plugins are loaded and
// all hooks are installed.  DLL_PROCESS_DETACH waits on this before running
// the shutdown sequence so it never tries to unload plugins that haven't been
// loaded yet.
static HANDLE g_pluginsLoadedEvent = NULL;

// Signalled (auto-reset) by OnEngineInitForUELog when the UE engine is fully
// up.  MainInitThreadProc waits on this before calling LoadAllPlugins so that
// plugins install their hooks into a fully initialised engine -- no thread
// suspension needed, no loader-lock deadlocks.
static HANDLE g_engineReadyEvent = NULL;

// Required suffix for the game version, read from the executable's version
static constexpr wchar_t kRequiredVersionSuffix[] = L"CL-115725";

#ifdef MODLOADER_CLIENT_BUILD
// Set during init from modloader.ini [UI] Enabled; read during shutdown.
static bool s_imguiEnabled = true;
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static DWORD WINAPI MainInitThreadProc(LPVOID);
static void LoadUE4SS();
static void LogStartupEnvironment();

// APC trampoline: fired on the game main thread when it enters an alertable
// wait.  We must NOT call MainInitThreadProc directly here -- it blocks on
// MsgWaitForMultipleObjects waiting for the EngineInit detour, which itself
// runs on this same (main) thread, causing a deadlock.  Instead we spawn a
// dedicated thread and return immediately so the main thread stays free to
// call FEngineLoop::Init (which triggers the hook we are waiting for).
static VOID CALLBACK MainInitApcProc(ULONG_PTR)
{
	g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
	if (!g_mainInitThread)
	{
		LogToFile::Error("FATAL: Failed to create main init thread from APC (%lu)", GetLastError());
		// Unblock DLL_PROCESS_DETACH so it does not hang waiting for plugins.
		if (g_pluginsLoadedEvent)
			SetEvent(g_pluginsLoadedEvent);
	}
}

// ---------------------------------------------------------------------------
// Called by EngineInit hook once the UE engine is up -- safe to call BasicLogV
// ---------------------------------------------------------------------------
static void OnEngineInitForUELog()
{
	// Initialise the UE log bridge so subsequent messages also appear in
	// StarRupture.log via BasicLogV.
	if (UELog::Initialize(+[](const std::string& pattern) -> uintptr_t {
		return Scanner::FindPatternInMainModule(std::string("BASIC_LOGV"), pattern);
		}))
	{
		LogToFile::Info("[ModLoader] UE log bridge active - messages will also appear in StarRupture.log");
	}

	// Load UE4SS on a background thread so that LoadLibraryW does not run
	// synchronously inside the engine-init hook detour.  Calling LoadLibraryW
	// from a hook callback acquires the loader lock while the engine (and GPU
	// driver threads) are still mid-initialisation, which can cause crashes in
	// nvoglv64 or similar drivers.  Deferring to a detached thread lets the
	// hook return cleanly first.
	std::thread([]()
	{
		// Give the engine init hook time to fully unwind and let any
		// concurrent driver initialisation settle.
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		LoadUE4SS();
	}).detach();
}

// ---------------------------------------------------------------------------
// Game version check
//
// Reads the "ProductVersion" string from the main executable's version
// resource and verifies that it ends with the expected changelist suffix.
// Returns true if the version is compatible; populates outActualVersion in
// both the success and failure cases so the caller can log it.
// ---------------------------------------------------------------------------

static bool CheckGameVersion(std::wstring& outActualVersion)
{
	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);

	DWORD dummy = 0;
	const DWORD infoSize = GetFileVersionInfoSizeW(exePath, &dummy);
	if (infoSize == 0)
	{
		outActualVersion = L"<unavailable>";
		return false;
	}

	std::vector<BYTE> buf(infoSize);
	if (!GetFileVersionInfoW(exePath, 0, infoSize, buf.data()))
	{
		outActualVersion = L"<unreadable>";
		return false;
	}

	// Use the first available translation to build the query path.
	struct LangCodepage { WORD lang; WORD codepage; };
	LangCodepage* translations = nullptr;
	UINT cbTranslations = 0;
	VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
	               reinterpret_cast<LPVOID*>(&translations), &cbTranslations);

	wchar_t query[64]{};
	if (translations && cbTranslations >= sizeof(LangCodepage))
		swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductVersion",
		           translations[0].lang, translations[0].codepage);
	else
		wcscpy_s(query, L"\\StringFileInfo\\040904b0\\ProductVersion");

	wchar_t* productVersion = nullptr;
	UINT versionLen = 0;
	if (!VerQueryValueW(buf.data(), query,
	                    reinterpret_cast<LPVOID*>(&productVersion), &versionLen)
	    || !productVersion || versionLen == 0)
	{
		outActualVersion = L"<not found>";
		return false;
	}

	outActualVersion = productVersion;

	const size_t reqLen    = wcslen(kRequiredVersionSuffix);
	const size_t actualLen = outActualVersion.size();
	return actualLen >= reqLen
	    && outActualVersion.compare(actualLen - reqLen, reqLen, kRequiredVersionSuffix) == 0;
}

// ---------------------------------------------------------------------------
// Main initialisation thread
//
// Owns everything that would previously have run inside DLL_PROCESS_ATTACH
// but cannot safely run there because:
//   * WinHTTP (used by RunAutoUpdate) lazily loads TLS/DNS DLLs via
//     LoadLibrary.  LoadLibrary needs the loader lock, which DllMain holds
//     for its entire duration -- causing an immediate deadlock if we wait, or
//     error 5023 if we try to use the thread pool from inside the lock.
//   * By running here we are already outside the loader lock, so WinHTTP,
//     hook installation, and plugin LoadLibrary calls all work normally.
//
// Plugin loading is deliberately deferred until the engine signals it is
// ready (via g_engineReadyEvent).  This guarantees that when PluginInit runs,
// the UE engine is fully initialised and it is safe for plugins to install
// hooks into game code.  Plugin OnEngineInit callbacks still fire: because
// the engine is already up by the time PluginInit is called, any call to
// IPluginHooks::RegisterEngineInitCallback inside PluginInit triggers the
// late-registration path in Hooks::EngineInit::RegisterPluginCallback, which
// invokes the callback immediately.
// ---------------------------------------------------------------------------
static DWORD WINAPI MainInitThreadProc(LPVOID)
{
	// Open the splash here, not in DllMain.  The splash window has no internal
	// message thread -- Show/SetStatus/SetProgress/Close must all be called from
	// the same thread that owns the HWND.  Running everything here keeps the
	// ownership consistent and avoids cross-thread UpdateWindow / PumpMessages
	// failures.  DllMain returns in microseconds so the visual delay is nil.

	Splash::Show();
	Splash::SetStatus(L"Starting mod loader...");
	Splash::SetProgress(0.0f);

	// Verify the game binary is the expected version before installing any hooks.
	// If the CL doesn't match we display a timed error and bail out cleanly.
	{
		std::wstring gameVersion;
		if (!CheckGameVersion(gameVersion))
		{
			LogToFile::Error("[ModLoader] VERSION MISMATCH: expected suffix '%ls', got '%ls'",
			                 kRequiredVersionSuffix, gameVersion.c_str());
			LogToFile::Error("[ModLoader] Hook installation aborted -- update to the correct game build.");

			// Only show the splash and countdown on client builds -- on server builds we just log the error and exit immediately since there's no UI to show it on.
#if defined(MODLOADER_CLIENT_BUILD)
			Splash::SetErrorMode(false);
			Splash::SetStatus(L"Wrong game version! Game will start without plugins & mod loader.");

			for (int countdown = 5; countdown > 0; --countdown)
			{
				wchar_t msg[256];

				Sleep(1000);
			}

			// Unblock DLL_PROCESS_DETACH so it doesn't hang waiting for init.
			if (g_pluginsLoadedEvent)
				SetEvent(g_pluginsLoadedEvent);

			Splash::Close();
#endif

			return 0;
		}

		LogToFile::Info("[ModLoader] Game version OK: %ls", gameVersion.c_str());
	}

	LogStartupEnvironment();

	Splash::SetStatus(L"Initializing logger...");
	Splash::SetProgress(0.20f);

	ModLoaderLogger::InitializeLogger();
	ModLoaderLogger::LogMessage(L"======================================");
	ModLoaderLogger::LogMessage(L"  AlienX's Mod Loader Starting");
	ModLoaderLogger::LogMessage(L"======================================");

	Splash::SetStatus(L"Here Goes Nothin' ...");
	Splash::SetProgress(0.30f);

	ModLoaderLogger::InitializeConfigManager();
	ModLoaderLogger::InitializePluginManager();

	// Auto-update runs here, outside the loader lock, so WinHTTP can work.
	// Downloaded DLLs are in place for this boot's plugin load.
	Splash::SetStatus(L"Checking for plugin updates...");
	Splash::SetProgress(0.35f);
	ModLoaderLogger::RunAutoUpdate();

	// Install core game hooks BEFORE loading plugins
	Splash::SetStatus(L"Installing core game hooks...");
	Splash::SetProgress(0.40f);

	ModLoaderLogger::LogMessage(L"Installing core game hooks...");
	// NOTE: WorldBeginPlay hook is installed lazily on first
	// RegisterAnyWorldBeginPlayCallback / RegisterWorldBeginPlayCallback call.

	Splash::SetStatus(L"Installing EngineInit hook...");
	Splash::SetProgress(0.50f);

	// Pass both events so the detour can signal engine-ready and then wait
	// for all plugins to load before letting the original Init proceed.
	Hooks::EngineInit::SetSyncEvents(g_engineReadyEvent, g_pluginsLoadedEvent);

	if (Hooks::EngineInit::Install())
	{
		ModLoaderLogger::LogDebug(L"  EngineInit hook installed");
		// OnEngineInitForUELog initialises the UE log bridge and loads UE4SS
		// after the original Init has returned.
		Hooks::EngineInit::RegisterPluginCallback(OnEngineInitForUELog);
	}
	else
	{
		ModLoaderLogger::LogWarn(L"  WARNING: EngineInit hook failed to install -- loading plugins immediately");
		// The detour will never fire to signal engine-ready, so unblock the
		// wait below manually so plugin loading can still proceed.
		if (g_engineReadyEvent)
			SetEvent(g_engineReadyEvent);
	}

	Splash::SetStatus(L"Installing EngineShutdown hook...");
	Splash::SetProgress(0.60f);

	if (Hooks::EngineShutdown::Install())
	{
		ModLoaderLogger::LogDebug(L"  EngineShutdown hook installed");
	}
	else
	{
		ModLoaderLogger::LogWarn(L"  WARNING: EngineShutdown hook failed to install - plugins will not receive shutdown callbacks");
	}

	Splash::SetStatus(L"Installing spawner hooks...");
	Splash::SetProgress(0.65f);	
	// Install spawner hooks eagerly now that pattern scanning is available.
	// These must be up before any plugin OnEngineInit callback runs so
	// plugins can rely on the hooks being present without race conditions.
	ModLoaderLogger::LogInfo(L"[EngineInit] Installing spawner hooks...");
	Hooks::MassSpawnerActivate::Install();
	Hooks::MassSpawnerDeactivate::Install();
	Hooks::MassDoSpawning::Install();

	// Wait for the engine to finish initialising before loading plugins.
	// We pump the thread message queue while waiting so the splash window
	// stays responsive.  A 2-minute timeout acts as a final safety net.
	Splash::SetStatus(L"Waiting for engine...");
	Splash::SetProgress(0.70f);

	LogToFile::Info("[ModLoader] Waiting for engine initialization...");

	if (g_engineReadyEvent)
	{
		static constexpr DWORD kEngineWaitTimeoutMs = 120'000; // 2 minutes
		HANDLE waitHandles[] = { g_engineReadyEvent };
		for (;;)
		{
			DWORD r = MsgWaitForMultipleObjects(1, waitHandles, FALSE, kEngineWaitTimeoutMs, QS_ALLINPUT);
			if (r == WAIT_OBJECT_0)
			{
				LogToFile::Info("[ModLoader] Engine ready -- proceeding to load plugins");
				break;
			}
			if (r == WAIT_OBJECT_0 + 1)
			{
				// Drain the message queue so the splash stays alive.
				MSG msg;
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
				continue;
			}
			// WAIT_TIMEOUT or unexpected error
			LogToFile::Warn("[ModLoader] Timed out waiting for engine init (%lu ms) -- loading plugins anyway", kEngineWaitTimeoutMs);
			break;
		}

		CloseHandle(g_engineReadyEvent);
		g_engineReadyEvent = NULL;
	}

	// Engine is up -- safe to load plugins and let them install hooks.
	Splash::SetStatus(L"Loading plugins...");
	Splash::SetProgress(0.80f);

#ifdef MODLOADER_CLIENT_BUILD
	// Install the keybind input processor so plugins can register keybinds
	// during their PluginInit call and have them active immediately.
	Hooks::Input::InstallInputProcessor();

	// Check modloader.ini [UI] Enabled before starting ImGui.
	// Allows users to disable the overlay entirely if it causes issues.
	// Write the default (1) back if the key doesn't exist yet so users can
	// see and edit the setting without having to know it exists.
	{
		wchar_t mlIniPath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, mlIniPath, MAX_PATH);
		wchar_t* lastSlash = wcsrchr(mlIniPath, L'\\');
		if (lastSlash)
			wcscpy_s(lastSlash + 1,
				static_cast<rsize_t>(MAX_PATH - (lastSlash + 1 - mlIniPath)),
				L"modloader.ini");

		// Use a sentinel default (-1) to detect whether the key is absent.
		int val = GetPrivateProfileIntW(L"UI", L"Enabled", -1, mlIniPath);
		if (val == -1)
		{
			WritePrivateProfileStringW(L"UI", L"Enabled", L"1", mlIniPath);
			val = 1;
		}
		s_imguiEnabled = (val != 0);
	}

	if (s_imguiEnabled)
	{
		// Initialize ImGui D3D12 backend.  This hooks IDXGISwapChain::Present,
		// reads the OpenKey from modloader.ini, and registers the internal toggle
		// keybind.  Must be called after the input processor is installed.
		UI::ImGuiBackend::Initialize();
	}

	// Delay D3D12 resource init until WorldBeginPlay fires.  By that point
	// Streamline and the UE5 viewport are fully stable, avoiding E_ABORT crashes.
	// Also show the overlay watermark only on the main menu world.
	static SDK::UWorld* s_currentWorld = nullptr;
	static auto s_onWorldReady = [](SDK::UWorld* world, const char* worldName)
	{
		s_currentWorld = world;
		if (s_imguiEnabled)
			UI::ImGuiBackend::SetRenderingReady();
		bool isMainMenu = worldName && strstr(worldName, "Map_MainMenu") != nullptr;
		UI::Overlay::SetVisible(isMainMenu);
		UI::GlobalSettings::SetWorldName(worldName ? worldName : "");
	};
	Hooks::WorldBeginPlay::RegisterAnyWorldCallback(s_onWorldReady);

	// Register a per-frame game-thread callback to read the local player's
	// position and cache it for the HUD overlay.  Called on the game thread
	// so SDK UFunction calls (ProcessEvent) are safe here.
	static auto s_onTick = [](float /*deltaSeconds*/)
	{
		SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(s_currentWorld, 0);
		if (!pc)
		{
			UI::GlobalSettings::SetPlayerPosition(0, 0, 0, false);
			return;
		}

		SDK::APawn* pawn = pc->K2_GetPawn();
		if (!pawn)
		{
			UI::GlobalSettings::SetPlayerPosition(0, 0, 0, false);
			return;
		}

		SDK::FVector loc = pawn->K2_GetActorLocation();
		UI::GlobalSettings::SetPlayerPosition(loc.X, loc.Y, loc.Z, true);
	};
	Hooks::EngineTick::RegisterPluginCallback(s_onTick);
#endif

	ModLoaderLogger::LoadAllPlugins();

	Splash::SetStatus(L"Initialization complete!");
	Splash::SetProgress(1.0f);

	ModLoaderLogger::LogInfo(L"Mod loader initialization complete");

	// Signal DLL_PROCESS_DETACH that init is complete and it is safe to start
	// the shutdown sequence (UnloadAllPlugins etc.).
	if (g_pluginsLoadedEvent)
		SetEvent(g_pluginsLoadedEvent);

	// Brief pause so the user can see 100%, then close the splash.
	Sleep(600);
	Splash::Close();

	return 0;
}

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

static void LogStartupEnvironment()
{
	LogToFile::Info("Process ID: %lu", GetCurrentProcessId());

	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	LogToFile::Info("Executable: %ls", exePath);

	wchar_t cwd[MAX_PATH]{};
	GetCurrentDirectoryW(MAX_PATH, cwd);
	LogToFile::Info("Working directory: %ls", cwd);

	LogToFile::Info("Command line: %ls", GetCommandLineW());

	HMODULE mainModule = GetModuleHandleW(nullptr);
	MODULEINFO mi{};
	if (GetModuleInformation(GetCurrentProcess(), mainModule, &mi, sizeof(mi)))
	{
		LogToFile::Info("Main module base: 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)));
		LogToFile::Info("Main module size: 0x%lX (%lu KB)", static_cast<unsigned long>(mi.SizeOfImage), mi.SizeOfImage / 1024);
		LogToFile::Info("Main module entry: 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.EntryPoint)));
	}
	else
	{
		LogToFile::Warn("Could not retrieve main module info");
	}

	if (IsWindows10OrGreater())           LogToFile::Info("OS version: Windows 10 or greater");
	else if (IsWindows8Point1OrGreater()) LogToFile::Info("OS version: Windows 8.1");
	else if (IsWindows8OrGreater())       LogToFile::Info("OS version: Windows 8");
	else if (IsWindows7OrGreater())       LogToFile::Info("OS version: Windows 7");
	else if (IsWindowsVistaOrGreater())   LogToFile::Info("OS version: Windows Vista");
	else                                  LogToFile::Info("OS version: Windows XP or older");

	LogToFile::Info("OS type: %s", IsWindowsServer() ? "Server" : "Client/Workstation");

	MEMORYSTATUSEX memStatus{};
	memStatus.dwLength = sizeof(memStatus);
	if (GlobalMemoryStatusEx(&memStatus))
	{
		LogToFile::Info("System RAM: %llu MB total, %llu MB available",
			memStatus.ullTotalPhys / (1024 * 1024),
			memStatus.ullAvailPhys / (1024 * 1024));
	}
}

static void LoadUE4SS()
{
	// Build the modloader.ini path next to the game exe
	wchar_t iniPath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
	wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
	if (lastSlash)
		wcscpy_s(lastSlash + 1,
			static_cast<rsize_t>(MAX_PATH - (lastSlash + 1 - iniPath)),
			L"modloader.ini");

	if (!GetPrivateProfileIntW(L"UE4SS", L"Enabled", 1, iniPath))
	{
		LogToFile::Info("UE4SS loading disabled in modloader.ini");
		return;
	}

	wchar_t relPath[MAX_PATH]{};
	GetPrivateProfileStringW(L"UE4SS", L"Path", L"ue4ss\\ue4ss.dll", relPath, MAX_PATH, iniPath);

	// Resolve relative to the exe directory
	wchar_t fullPath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, fullPath, MAX_PATH);
	wchar_t* slash = wcsrchr(fullPath, L'\\');
	if (slash) *(slash + 1) = L'\0';
	wcsncat_s(fullPath, relPath, _TRUNCATE);

	LogToFile::Info("Loading UE4SS from: %ls", fullPath);

	HMODULE hUE4SS = LoadLibraryW(fullPath);
	if (hUE4SS)
	{
		LogToFile::Info("UE4SS loaded successfully (handle 0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hUE4SS)));
	}
	else
	{
		LogToFile::Warn("UE4SS failed to load (error %lu): %ls", GetLastError(), fullPath);
	}
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);

		// Call DBGHelp to refresh the module list and ensure our own symbols are available for stack traces from the very start.
		SymRefreshModuleList(GetCurrentProcess());


		// Initialise our low-level file logger -- simple file I/O, no LoadLibrary.
		LogToFile::Initialize();
		LogToFile::Info("======================================================");
		LogToFile::Info("  StarRupture Mod Loader (dwmapi.dll proxy) loaded");
		LogToFile::Info("======================================================");

		// Initialise the dwmapi forwarding table synchronously.  This MUST
		// happen in DllMain before any caller can reach our dwmapi exports,
		// and before we release the loader lock.
		// NOTE: Splash::Show() is intentionally NOT called here.  The splash
		// uses a plain HWND with no internal thread -- all calls (Show, SetStatus,
		// SetProgress, Close) must be made from the same thread that owns the
		// window.  We open the splash at the top of MainInitThreadProc so that
		// every call originates from that one thread.

		LogToFile::Info("Initializing dwmapi.dll proxy...");
		if (!DwmapiProxy::Initialize())
		{
			LogToFile::Error("FATAL: Failed to initialize dwmapi proxy -- DLL load aborted");
			LogToFile::Shutdown();
			return FALSE;
		}
		LogToFile::Info("Dwmapi proxy initialized successfully");

		// Create the engine-ready event before spawning the init thread so
		// there is no race between the thread registering the callback and
		// OnEngineInitForUELog trying to signal the event.
		g_engineReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!g_engineReadyEvent)
		{
			LogToFile::Error("FATAL: Failed to create engine-ready event (%lu)", GetLastError());
			DwmapiProxy::Shutdown();
			LogToFile::Shutdown();
			return FALSE;
		}

		// Create the synchronisation event used by DLL_PROCESS_DETACH to wait
		// for the init thread before running the shutdown sequence.
		g_pluginsLoadedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!g_pluginsLoadedEvent)
		{
			LogToFile::Error("FATAL: Failed to create init-done event (%lu)", GetLastError());
			CloseHandle(g_engineReadyEvent);
			g_engineReadyEvent = NULL;
			DwmapiProxy::Shutdown();
			LogToFile::Shutdown();
			return FALSE;
		}

		// If DllMain is running on the game's main thread, queue an APC so
		// MainInitThreadProc fires the next time that thread enters an alertable
		// wait (after the loader lock is released).  Otherwise spawn a dedicated
		// thread as normal -- the spawned thread is never the main thread, so
		// it won't contend with game logic running there.
		if (GetCurrentThreadId() == get_main_thread_id())
		{
			LogToFile::Info("DllMain on main thread -- deferring init via QueueUserAPC");
			QueueUserAPC((PAPCFUNC)MainInitApcProc, GetCurrentThread(), (ULONG_PTR)hModule);
			// g_mainInitThread stays NULL; DLL_PROCESS_DETACH guards with if()
		}
		else
		{
			g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
			if (!g_mainInitThread)
			{
				DWORD err = GetLastError();
				LogToFile::Error("FATAL: Failed to create main init thread (%lu)", err);
				CloseHandle(g_pluginsLoadedEvent);
				g_pluginsLoadedEvent = NULL;
				CloseHandle(g_engineReadyEvent);
				g_engineReadyEvent = NULL;
				DwmapiProxy::Shutdown();
				LogToFile::Shutdown();
				return FALSE;
			}
		}

		// Return TRUE immediately -- loader lock is now released and the init
		// work can proceed (either via the APC or the spawned thread).
	}
	break;

	case DLL_PROCESS_DETACH:
	{
		// lpReserved is non-null when the process is terminating (as opposed to
		// an explicit FreeLibrary call). During process termination the engine's
		// allocator, object system and hooks are already in a partially destroyed
		// state - calling into them from DllMain (which holds the loader lock)
		// causes MallocBinned2 canary corruption and similar crashes. Let the OS
		// reclaim everything; we do not need to clean up in that case.
		if (lpReserved != nullptr)
		{
			LogToFile::Info("Process terminating - skipping shutdown to avoid loader-lock / allocator corruption");
			LogToFile::Shutdown();
			break;
		}

		// Clean up the engine-ready event if the init thread didn't get to it
		// (e.g. if FreeLibrary is called very early).
		if (g_engineReadyEvent)
		{
			CloseHandle(g_engineReadyEvent);
			g_engineReadyEvent = NULL;
		}

		// Wait for the init thread to finish so we never try to unload plugins
		// that haven't been loaded yet, or tear down hooks before they are set.
		// g_pluginsLoadedEvent is signalled (and doesn't need LoadLibrary to be
		// waited on), so there is no deadlock risk even though the loader lock
		// is held here.
		if (g_pluginsLoadedEvent)
		{
			if (WaitForSingleObject(g_pluginsLoadedEvent, 30000) == WAIT_TIMEOUT)
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

		// CRITICAL: Remove engine shutdown hook and clear callbacks FIRST
		// This prevents the hook from firing with dangling function pointers
		// after plugins are unloaded
		ModLoaderLogger::LogInfo(L"Removing engine shutdown hook...");
		Hooks::EngineShutdown::Remove();
		ModLoaderLogger::LogInfo(L"Engine shutdown hook removed");

		// Now safe to unload plugins
		ModLoaderLogger::UnloadAllPlugins();

		// Remove remaining core game hooks
		ModLoaderLogger::LogInfo(L"Removing remaining core game hooks...");
		Hooks::EngineInit::Remove();
		Hooks::WorldBeginPlay::Remove();
		Hooks::SaveLoaded::Remove();
		Hooks::ExperienceLoadComplete::Remove();
		Hooks::ActorBeginPlay::Remove();
		Hooks::PlayerJoined::Remove();
		Hooks::PlayerLeft::Remove();
		Hooks::MassSpawnerActivate::Remove();
		Hooks::MassSpawnerDeactivate::Remove();
		Hooks::MassDoSpawning::Remove();
#ifdef MODLOADER_CLIENT_BUILD
		if (s_imguiEnabled)
			UI::ImGuiBackend::Shutdown();
		Hooks::Input::RemoveInputProcessor();
#endif

		ModLoaderLogger::ShutdownPluginManager();
		ModLoaderLogger::ShutdownConfigManager();
		ModLoaderLogger::ShutdownLogger();

		ModLoaderLogger::LogInfo(L"Shutting down dwmapi proxy...");
		DwmapiProxy::Shutdown();

		ModLoaderLogger::LogInfo(L"Goodbye!");
		ModLoaderLogger::ShutdownLogger();
	}
	break;

	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return TRUE;
}
