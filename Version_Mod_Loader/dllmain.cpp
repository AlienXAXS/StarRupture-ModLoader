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

// Forward declarations
static void LoadUE4SS();

// Called by EngineInit hook once the UE engine is up — safe to call BasicLogV
static void OnEngineInitForUELog()
{
	if (UELog::Initialize(Scanner::FindPatternInMainModule))
	{
		Log::Info("[ModLoader] UE log bridge active - messages will also appear in StarRupture.log");
	}
	else
	{
		Log::Warn("[ModLoader] UE log bridge failed to initialize - BasicLogV pattern not found");
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

static void LogStartupEnvironment()
{
	Log::Info("Process ID: %lu", GetCurrentProcessId());

	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	Log::Info("Executable: %ls", exePath);

	wchar_t cwd[MAX_PATH]{};
	GetCurrentDirectoryW(MAX_PATH, cwd);
	Log::Info("Working directory: %ls", cwd);

	Log::Info("Command line: %ls", GetCommandLineW());

	HMODULE mainModule = GetModuleHandleW(nullptr);
	MODULEINFO mi{};
	if (GetModuleInformation(GetCurrentProcess(), mainModule, &mi, sizeof(mi)))
	{
		Log::Info("Main module base: 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)));
		Log::Info("Main module size: 0x%lX (%lu KB)", static_cast<unsigned long>(mi.SizeOfImage), mi.SizeOfImage / 1024);
		Log::Info("Main module entry: 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.EntryPoint)));
	}
	else
	{
		Log::Warn("Could not retrieve main module info");
	}

	if (IsWindows10OrGreater())           Log::Info("OS version: Windows 10 or greater");
	else if (IsWindows8Point1OrGreater()) Log::Info("OS version: Windows 8.1");
	else if (IsWindows8OrGreater())       Log::Info("OS version: Windows 8");
	else if (IsWindows7OrGreater())       Log::Info("OS version: Windows 7");
	else if (IsWindowsVistaOrGreater())   Log::Info("OS version: Windows Vista");
	else                                  Log::Info("OS version: Windows XP or older");

	Log::Info("OS type: %s", IsWindowsServer() ? "Server" : "Client/Workstation");

	MEMORYSTATUSEX memStatus{};
	memStatus.dwLength = sizeof(memStatus);
	if (GlobalMemoryStatusEx(&memStatus))
	{
		Log::Info("System RAM: %llu MB total, %llu MB available",
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
		Log::Info("UE4SS loading disabled in modloader.ini");
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

	Log::Info("Loading UE4SS from: %ls", fullPath);

	HMODULE hUE4SS = LoadLibraryW(fullPath);
	if (hUE4SS)
	{
		Log::Info("UE4SS loaded successfully (handle 0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hUE4SS)));
	}
	else
	{
		Log::Warn("UE4SS failed to load (error %lu): %ls", GetLastError(), fullPath);
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);

		Log::Initialize();
		Log::Info("======================================================");
		Log::Info("  StarRupture Mod Loader (dwmapi.dll proxy) loaded");
		Log::Info("======================================================");

		// Show splash window on client builds (no-op on server)
		Splash::Show();
		Splash::SetStatus(L"Starting mod loader...");
		Splash::SetProgress(0.0f);

		LogStartupEnvironment();

		Splash::SetStatus(L"Initializing dwmapi proxy...");
		Splash::SetProgress(0.10f);

		Log::Info("Initializing dwmapi.dll proxy...");
		if (!DwmapiProxy::Initialize())
		{
			Log::Error("FATAL: Failed to initialize dwmapi proxy -- DLL load aborted");
			Splash::Close();
			Log::Shutdown();
			return FALSE;
		}
		Log::Info("Dwmapi proxy initialized successfully");

		Splash::SetStatus(L"Initializing logger...");
		Splash::SetProgress(0.20f);

		ModLoader::InitializeLogger();
		ModLoader::LogMessage(L"======================================");
		ModLoader::LogMessage(L"  AlienX's Mod Loader initialized");
		ModLoader::LogMessage(L"======================================");

		Splash::SetStatus(L"Here Goes Nothin' ...");
		Splash::SetProgress(0.30f);

		ModLoader::InitializeConfigManager();
		ModLoader::InitializePluginManager();

		Splash::SetStatus(L"Checking for plugin updates...");
		Splash::SetProgress(0.35f);
		ModLoader::RunAutoUpdate();

		// Install core game hooks BEFORE loading plugins
		Splash::SetStatus(L"Installing core game hooks...");
		Splash::SetProgress(0.40f);

		ModLoader::LogMessage(L"Installing core game hooks...");
		// NOTE: WorldBeginPlay hook is installed lazily on first
		// RegisterAnyWorldBeginPlayCallback / RegisterWorldBeginPlayCallback call.

		Splash::SetStatus(L"Installing EngineInit hook...");
		Splash::SetProgress(0.55f);

		if (Hooks::EngineInit::Install())
		{
			ModLoader::LogMessage(L"  EngineInit hook installed");
			// Register UE log bridge — initialised once the engine is up
			Hooks::EngineInit::RegisterPluginCallback(OnEngineInitForUELog);
		}
		else
		{
			ModLoader::LogMessage(L"  WARNING: EngineInit hook failed to install");
		}

		Splash::SetStatus(L"Installing EngineShutdown hook...");
		Splash::SetProgress(0.65f);

		if (Hooks::EngineShutdown::Install())
		{
			ModLoader::LogMessage(L"  EngineShutdown hook installed");
		}
		else
		{
			ModLoader::LogMessage(L"  WARNING: EngineShutdown hook failed to install - plugins will not receive shutdown callbacks");
		}

		Splash::SetStatus(L"Loading plugins...");
		Splash::SetProgress(0.75f);

		ModLoader::LoadAllPlugins();

		Splash::SetStatus(L"Initialization complete!");
		Splash::SetProgress(1.0f);

		ModLoader::LogMessage(L"Mod loader initialization complete");

		// Brief pause so the user can see 100%, then close
		Sleep(600);
		Splash::Close();
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
			Log::Info("Process terminating - skipping shutdown to avoid loader-lock / allocator corruption");
			Log::Shutdown();
			break;
		}

		ModLoader::LogMessage(L"======================================");
		ModLoader::LogMessage(L"  Version_Mod_Loader shutting down");
		ModLoader::LogMessage(L"======================================");


		// CRITICAL: Remove engine shutdown hook and clear callbacks FIRST
		// This prevents the hook from firing with dangling function pointers
		// after plugins are unloaded
		ModLoader::LogMessage(L"Removing engine shutdown hook...");
		Hooks::EngineShutdown::Remove();
		ModLoader::LogMessage(L"Engine shutdown hook removed");

		// Now safe to unload plugins
		ModLoader::UnloadAllPlugins();

		// Remove remaining core game hooks
		ModLoader::LogMessage(L"Removing remaining core game hooks...");
		Hooks::EngineInit::Remove();
		Hooks::WorldBeginPlay::Remove();
		Hooks::SaveLoaded::Remove();
		Hooks::ExperienceLoadComplete::Remove();
		Hooks::ActorBeginPlay::Remove();
		Hooks::PlayerJoined::Remove();
		Hooks::PlayerLeft::Remove();

		ModLoader::ShutdownPluginManager();
		ModLoader::ShutdownConfigManager();
		ModLoader::ShutdownLogger();

		Log::Info("Shutting down dwmapi proxy...");
		DwmapiProxy::Shutdown();

		Log::Info("Goodbye!");
		Log::Shutdown();
	}
	break;

	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return TRUE;
}
