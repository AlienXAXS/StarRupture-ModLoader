// dllmain.cpp : Defines the entry point for the DLL application.
#include "log.h"
#include "ue_log.h"
#include "version_proxy.h"
#include "logger.h"
#include "config_manager.h"
#include "plugin_manager.h"
#include "scanner.h"
#include "splash_window.h"
#include "game/world_begin_play/world_begin_play.h"
#include "game/engine_init/engine_init.h"
#include "game/engine_shutdown/engine_shutdown.h"
#include "game/save_loaded/save_loaded.h"
#include "game/experience_load_complete/experience_load_complete.h"
#include <Psapi.h>
#include <VersionHelpers.h>

#pragma comment(lib, "psapi.lib")

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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);

		Log::Initialize();
		Log::Info("======================================================");
		Log::Info("  StarRupture Mod Loader (version.dll proxy) loaded");
		Log::Info("======================================================");

		// Show splash window on client builds (no-op on server)
		Splash::Show();
		Splash::SetStatus(L"Starting mod loader...");
		Splash::SetProgress(0.0f);

		LogStartupEnvironment();

		Splash::SetStatus(L"Initializing version proxy...");
		Splash::SetProgress(0.10f);

		Log::Info("Initializing version.dll proxy...");
		if (!VersionProxy::Initialize())
		{
			Log::Error("FATAL: Failed to initialize version proxy -- DLL load aborted");
			Splash::Close();
			Log::Shutdown();
			return FALSE;
		}
		Log::Info("Version proxy initialized successfully");

		Splash::SetStatus(L"Initializing logger...");
		Splash::SetProgress(0.20f);

		ModLoader::InitializeLogger();
		ModLoader::LogMessage(L"======================================");
		ModLoader::LogMessage(L"  Version_Mod_Loader initialized");
		ModLoader::LogMessage(L"======================================");

		Splash::SetStatus(L"Initializing config & plugin manager...");
		Splash::SetProgress(0.30f);

		ModLoader::InitializeConfigManager();
		ModLoader::InitializePluginManager();

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

		ModLoader::ShutdownPluginManager();
		ModLoader::ShutdownConfigManager();
		ModLoader::ShutdownLogger();

		Log::Info("Shutting down version proxy...");
		VersionProxy::Shutdown();

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
