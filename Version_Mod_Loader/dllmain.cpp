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

#include "auto_update/auto_updater.h"


#include <Psapi.h>
#include <VersionHelpers.h>
#include <thread>
#include <chrono>

#pragma comment(lib, "psapi.lib")

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

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void LoadUE4SS();
static void LogStartupEnvironment();

// ---------------------------------------------------------------------------
// Called by EngineInit hook once the UE engine is up — safe to call BasicLogV
// ---------------------------------------------------------------------------
static void OnEngineInitForUELog()
{
	if (UELog::Initialize(Scanner::FindPatternInMainModule))
	{
		LogToFile::Info("[ModLoader] UE log bridge active - messages will also appear in StarRupture.log");
	}
	else
	{
		LogToFile::Warn("[ModLoader] UE log bridge failed to initialize - BasicLogV pattern not found");
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
// Main initialisation thread
//
// Owns everything that would previously have run inside DLL_PROCESS_ATTACH
// but cannot safely run there because:
//   • WinHTTP (used by RunAutoUpdate) lazily loads TLS/DNS DLLs via
//     LoadLibrary.  LoadLibrary needs the loader lock, which DllMain holds
//     for its entire duration — causing an immediate deadlock if we wait, or
//     error 5023 if we try to use the thread pool from inside the lock.
//   • By running here we are already outside the loader lock, so WinHTTP,
//     hook installation, and plugin LoadLibrary calls all work normally.
// ---------------------------------------------------------------------------
static DWORD WINAPI MainInitThreadProc(LPVOID)
{
	// Open the splash here, not in DllMain.  The splash window has no internal
	// message thread — Show/SetStatus/SetProgress/Close must all be called from
	// the same thread that owns the HWND.  Running everything here keeps the
	// ownership consistent and avoids cross-thread UpdateWindow / PumpMessages
	// failures.  DllMain returns in microseconds so the visual delay is nil.
	Splash::Show();
	Splash::SetStatus(L"Starting mod loader...");
	Splash::SetProgress(0.0f);

	LogStartupEnvironment();

	Splash::SetStatus(L"Initializing logger...");
	Splash::SetProgress(0.20f);

	ModLoaderLogger::InitializeLogger();
	ModLoaderLogger::LogMessage(L"======================================");
	ModLoaderLogger::LogMessage(L"  AlienX's Mod Loader initialized");
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
	Splash::SetProgress(0.55f);

	if (Hooks::EngineInit::Install())
	{
		ModLoaderLogger::LogDebug(L"  EngineInit hook installed");
		// Register UE log bridge — initialised once the engine is up
		Hooks::EngineInit::RegisterPluginCallback(OnEngineInitForUELog);
	}
	else
	{
		ModLoaderLogger::LogWarn(L"  WARNING: EngineInit hook failed to install");
	}

	Splash::SetStatus(L"Installing EngineShutdown hook...");
	Splash::SetProgress(0.65f);

	if (Hooks::EngineShutdown::Install())
	{
		ModLoaderLogger::LogDebug(L"  EngineShutdown hook installed");
	}
	else
	{
		ModLoaderLogger::LogWarn(L"  WARNING: EngineShutdown hook failed to install - plugins will not receive shutdown callbacks");
	}

	Splash::SetStatus(L"Loading plugins...");
	Splash::SetProgress(0.75f);

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

		// Initialise our low-level file logger — simple file I/O, no LoadLibrary.
		LogToFile::Initialize();
		LogToFile::Info("======================================================");
		LogToFile::Info("  StarRupture Mod Loader (dwmapi.dll proxy) loaded");
		LogToFile::Info("======================================================");

		// Initialise the dwmapi forwarding table synchronously.  This MUST
		// happen in DllMain before any caller can reach our dwmapi exports,
		// and before we release the loader lock.
		// NOTE: Splash::Show() is intentionally NOT called here.  The splash
		// uses a plain HWND with no internal thread — all calls (Show, SetStatus,
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

		// Create the synchronisation event used by DLL_PROCESS_DETACH to wait
		// for the init thread before running the shutdown sequence.
		g_pluginsLoadedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!g_pluginsLoadedEvent)
		{
			LogToFile::Error("FATAL: Failed to create init-done event (%lu)", GetLastError());
			DwmapiProxy::Shutdown();
			LogToFile::Shutdown();
			return FALSE;
		}

		// Spawn the main init thread and return immediately, releasing the
		// loader lock.  Everything else (splash, auto-update, hooks, plugin
		// loading) runs in that thread where WinHTTP and LoadLibrary work
		// normally, and where all splash calls share the same owning thread.
		g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
		if (!g_mainInitThread)
		{
			DWORD err = GetLastError();
			LogToFile::Error("FATAL: Failed to create main init thread (%lu)", err);
			CloseHandle(g_pluginsLoadedEvent);
			g_pluginsLoadedEvent = NULL;
			DwmapiProxy::Shutdown();
			LogToFile::Shutdown();
			return FALSE;
		}

		// Return TRUE immediately — loader lock is now released and the init
		// thread can proceed with WinHTTP, hook patching, and plugin loading.
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

		// Wait for the init thread to finish so we never try to unload plugins
		// that haven't been loaded yet, or tear down hooks before they are set.
		// g_pluginsLoadedEvent is signalled (and doesn't need LoadLibrary to be
		// waited on), so there is no deadlock risk even though the loader lock
		// is held here.
		if (g_pluginsLoadedEvent)
		{
			if (WaitForSingleObject(g_pluginsLoadedEvent, 30000) == WAIT_TIMEOUT)
				LogToFile::Warn("Timed out waiting for init thread — proceeding with shutdown anyway");
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
