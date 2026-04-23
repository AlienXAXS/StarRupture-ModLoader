// dllmain.cpp : Defines the entry point for the DLL application.
#include "dwmapi_proxy.h"
#include "config/config_manager.h"
#include "hooks/game/actor_begin_play/actor_begin_play.h"
#include "hooks/game/engine_init/engine_init.h"
#include "hooks/game/engine_shutdown/engine_shutdown.h"
#include "hooks/game/experience_load_complete/experience_load_complete.h"
#include "hooks/game/game_instance_init/game_instance_init.h"
#include "hooks/game/mass_do_spawning/mass_do_spawning.h"
#include "hooks/game/mass_spawner_activate/mass_spawner_activate.h"
#include "hooks/game/mass_spawner_deactivate/mass_spawner_deactivate.h"
#include "hooks/game/player_joined/player_joined.h"
#include "hooks/game/player_left/player_left.h"
#include "hooks/game/save_loaded/save_loaded.h"
#include "hooks/game/world_begin_play/world_begin_play.h"
#include "logging/log.h"
#include "logging/logger.h"
#include "logging/ue_log.h"
#include "memory_scanner/scanner.h"
#include "network_channel/network_channel.h"
#include "plugins/plugin_manager.h"
#include "UI/splash_window.h"

#ifdef MODLOADER_SERVER_BUILD
#include "hooks/http/http_server_hook.h"
#endif

#include "auto_update/auto_updater.h"
#include "utils/thread_utils.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "Engine_classes.hpp"
#include "hooks/game/engine_tick/engine_tick.h"
#include "hooks/game/hud_post_render/hud_post_render.h"
#include "hooks/input/input_processor.h"
#include "UI/global_settings.h"
#include "UI/imgui_backend.h"
#include "UI/overlay.h"
#endif

#include "DbgHelp.h"
#pragma comment(lib, "DbgHelp.lib")

#include <chrono>
#include <Psapi.h>
#include <thread>
#include <vector>
#include <VersionHelpers.h>
#include <winver.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

// ---------------------------------------------------------------------------
// Globals shared between DllMain and the main init thread
// ---------------------------------------------------------------------------

// Handle to the main initialisation thread (kept so DLL_PROCESS_DETACH can
// wait for it in the FreeLibrary case before tearing down subsystems).
static HANDLE g_mainInitThread   = NULL;
static HANDLE g_autoUpdateThread = NULL;

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

// Signalled (auto-reset) at the very end of the EngineInit detour, after
// NotifyEngineReady has returned and the hook call-stack has fully unwound.
// The UE4SS loader thread waits on this instead of a fixed sleep so it never
// calls LoadLibraryW while the detour or GPU driver init is still active.
static HANDLE g_ue4ssReadyEvent = NULL;

// Required suffix for the game version, read from the executable's version resource.
static constexpr wchar_t kRequiredVersionSuffix[] = L"CL-118961";

#ifdef MODLOADER_CLIENT_BUILD
// Set during init from modloader.ini [UI] Enabled; read during shutdown.
static bool s_imguiEnabled = true;

// Current world pointer -- written by the WorldBeginPlay callback, read by the
// per-tick callback.  Both run on the game thread so no locking is needed.
static SDK::UWorld* s_currentWorld = nullptr;
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static DWORD WINAPI MainInitThreadProc(LPVOID);
static DWORD WINAPI AutoUpdateThreadProc(LPVOID);
static void         InitSubsystems();
static void         InstallHooksPhase();
static void         WaitForEnginePhase();
static void         LoadPluginsPhase();
static void         InitPluginsIfReady();
static void         LoadUE4SS();
static void         LogStartupEnvironment();
static bool         VerifyGameVersion();
static void         InstallAllHooks();
static void         RemoveAllHooks();
static void         WaitForEngineReady();
#ifdef MODLOADER_CLIENT_BUILD
static void         InitClientUI();
static void         ShutdownClientUI();
#endif
static void         ShutdownAll();

// ---------------------------------------------------------------------------
// Path utility: returns the directory containing the game executable,
// with a trailing backslash, optionally with a filename appended.
// ---------------------------------------------------------------------------
static std::wstring GetExeDir()
{
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash) *(slash + 1) = L'\0';
	return path;
}

static std::wstring GetExeDirPath(const wchar_t* filename)
{
	return GetExeDir() + filename;
}

// ---------------------------------------------------------------------------
// APC trampoline: fired on the game main thread when it enters an alertable
// wait.  We must NOT call MainInitThreadProc directly here -- it blocks on
// MsgWaitForMultipleObjects waiting for the EngineInit detour, which itself
// runs on this same (main) thread, causing a deadlock.  Instead we spawn a
// dedicated thread and return immediately so the main thread stays free to
// call FEngineLoop::Init (which triggers the hook we are waiting for).
// ---------------------------------------------------------------------------
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
	// synchronously inside the engine-init hook detour.  The thread waits on
	// g_ue4ssReadyEvent, which is signalled at the very end of the detour
	// (after NotifyEngineReady returns), guaranteeing the hook call-stack has
	// fully unwound and GPU driver threads spawned during FEngineLoop::Init
	// have had time to settle before we acquire the loader lock for UE4SS.
	std::thread([]()
	{
		if (g_ue4ssReadyEvent)
		{
			constexpr DWORD kTimeoutMs = 15'000;
			DWORD r = WaitForSingleObject(g_ue4ssReadyEvent, kTimeoutMs);
			if (r == WAIT_TIMEOUT)
				LogToFile::Warn("UE4SS load: timed out waiting for detour to unwind (%lu ms) -- loading anyway", kTimeoutMs);
			else if (r != WAIT_OBJECT_0)
				LogToFile::Warn("UE4SS load: WaitForSingleObject returned unexpected value %lu -- loading anyway", r);
		}
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
// Version verification with splash feedback on mismatch.
// Returns true if init should continue, false if it should abort.
// ---------------------------------------------------------------------------
static bool VerifyGameVersion()
{
	std::wstring gameVersion;

#if _DEBUG
	const bool versionOk = true;
#else
	const bool versionOk = CheckGameVersion(gameVersion);
#endif

	if (!versionOk)
	{
		LogToFile::Error("[ModLoader] VERSION MISMATCH: expected suffix '%ls', got '%ls'",
		 kRequiredVersionSuffix, gameVersion.c_str());
		LogToFile::Error("[ModLoader] Hook installation aborted -- update to the correct game build.");

		// On client builds, show a visible countdown before closing the splash.
		// On server builds there is no UI, so we just log and fall through to abort.
#ifdef MODLOADER_CLIENT_BUILD
		Splash::SetErrorMode(false);
		for (int countdown = 5; countdown > 0; --countdown)
		{
			wchar_t msg[64];
			swprintf_s(msg, L"Wrong game version! Closing in %d...", countdown);
			Splash::SetStatus(msg);
			Sleep(1000);
		}
		Splash::Close();
#endif

		// Unblock DLL_PROCESS_DETACH on all build types.
		if (g_pluginsLoadedEvent)
			SetEvent(g_pluginsLoadedEvent);

		return false;
	}

	LogToFile::Info("[ModLoader] Game version OK: %ls", gameVersion.c_str());
	return true;
}

// ---------------------------------------------------------------------------
// Install all core game hooks.
// Called once from MainInitThreadProc after early init is complete.
// ---------------------------------------------------------------------------
static void InstallAllHooks()
{
	ModLoaderLogger::LogMessage(L"Installing core game hooks...");
	// NOTE: WorldBeginPlay hook is installed lazily on first
	// RegisterAnyWorldBeginPlayCallback / RegisterWorldBeginPlayCallback call.

	Splash::SetStatus(L"Installing EngineInit hook...");
	Splash::SetProgress(0.20f);

	// Pass both events so the detour can signal engine-ready and then wait
	// for all plugins to load before letting the original Init proceed.
	Hooks::EngineInit::SetSyncEvents(g_engineReadyEvent, g_pluginsLoadedEvent);

	// Pass the UE4SS-ready event so the detour can signal it once its
	// call-stack has fully unwound, letting the UE4SS loader thread proceed.
	Hooks::EngineInit::SetUE4SSReadyEvent(g_ue4ssReadyEvent);

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
	Splash::SetProgress(0.25f);

	if (Hooks::EngineShutdown::Install())
		ModLoaderLogger::LogDebug(L"  EngineShutdown hook installed");
	else
		ModLoaderLogger::LogWarn(L"  WARNING: EngineShutdown hook failed to install - plugins will not receive shutdown callbacks");

	Splash::SetStatus(L"Installing spawner hooks...");
	Splash::SetProgress(0.30f);

	// Install spawner hooks eagerly now that pattern scanning is available.
	// These must be up before any plugin OnEngineInit callback runs so
	// plugins can rely on the hooks being present without race conditions.
	ModLoaderLogger::LogInfo(L"[EngineInit] Installing spawner hooks...");
	Hooks::MassSpawnerActivate::Install();
	Hooks::MassSpawnerDeactivate::Install();
	Hooks::MassDoSpawning::Install();

#ifdef MODLOADER_SERVER_BUILD
	Splash::SetStatus(L"Installing HTTP server hook...");
	if (Hooks::HttpServer::Install())
		ModLoaderLogger::LogDebug(L"  HttpServer hook installed");
	else
		ModLoaderLogger::LogWarn(L"  WARNING: HttpServer hook failed — static file routes and request filters will not function");
#endif

	Splash::SetStatus(L"Installing GameInstance hook...");
	Splash::SetProgress(0.40f);

	// Install UGameInstance::Init hook.  Pattern scanning works at any time
	// (reads .text section), so we install early here.  The detour calls
	// InitAllLoadedPlugins() after the original returns, which is the first
	// point where GObjects is fully populated and safe for UFunction lookups.
	if (Hooks::GameInstanceInit::Install())
		ModLoaderLogger::LogDebug(L"  GameInstanceInit hook installed");
	else
		ModLoaderLogger::LogWarn(L"  WARNING: GameInstanceInit hook failed -- plugins will not be initialized");
}

// ---------------------------------------------------------------------------
// Remove all core game hooks.
// Called during shutdown before subsystems are torn down.
// ---------------------------------------------------------------------------
static void RemoveAllHooks()
{
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
#ifdef MODLOADER_SERVER_BUILD
	Hooks::HttpServer::Remove();
#endif
}

// ---------------------------------------------------------------------------
// Wait for the engine to signal it is ready, pumping messages meanwhile so
// the splash window stays responsive.  Two-minute timeout as a safety net.
// ---------------------------------------------------------------------------
static void WaitForEngineReady()
{
	if (!g_engineReadyEvent)
		return;

	Splash::SetStatus(L"Waiting for engine...");
	Splash::SetProgress(0.45f);
	LogToFile::Info("[ModLoader] Waiting for engine initialization...");

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

// ---------------------------------------------------------------------------
// Client-only UI initialisation / shutdown
// ---------------------------------------------------------------------------
#ifdef MODLOADER_CLIENT_BUILD

static void InitClientUI()
{
	// Read [UI] Enabled from modloader.ini.  Write the default (1) back if
	// the key doesn't exist yet so users can discover and edit the setting.
	const std::wstring iniPath = GetExeDirPath(L"modloader.ini");
	int val = GetPrivateProfileIntW(L"UI", L"Enabled", -1, iniPath.c_str());
	if (val == -1)
	{
		WritePrivateProfileStringW(L"UI", L"Enabled", L"1", iniPath.c_str());
		val = 1;
	}
	s_imguiEnabled = (val != 0);

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
	static auto s_onWorldReady = [](SDK::UWorld* world, const char* worldName)
	{
		s_currentWorld = world;

		//if (s_imguiEnabled)
		//	UI::ImGuiBackend::SetRenderingReady();

		const bool isMainMenu = worldName && strstr(worldName, "Map_MainMenu") != nullptr;
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
}

static void ShutdownClientUI()
{
	if (s_imguiEnabled)
		UI::ImGuiBackend::Shutdown();
	Hooks::Input::RemoveInputProcessor();
}

#endif // MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Full shutdown sequence.
// Only called on an explicit FreeLibrary -- process-exit teardown is skipped
// entirely (lpReserved != nullptr is handled in DLL_PROCESS_DETACH before
// this is reached) to avoid loader-lock / allocator-corruption crashes.
// ---------------------------------------------------------------------------
static void ShutdownAll()
{
	// If auto-update thread is still running (e.g. FreeLibrary before engine ready),
	// wait for it so we don't tear down WinHTTP while it's in use.
	if (g_autoUpdateThread)
	{
		WaitForSingleObject(g_autoUpdateThread, 15'000);
		CloseHandle(g_autoUpdateThread);
		g_autoUpdateThread = NULL;
	}

	// Release any synchronisation handles the init thread may not have
	// cleaned up (e.g. FreeLibrary called before engine ever started).
	if (g_engineReadyEvent)
	{
		CloseHandle(g_engineReadyEvent);
		g_engineReadyEvent = NULL;
	}

	// Signal the UE4SS background thread (if still waiting) before closing
	// the handle so it can unblock and exit cleanly.
	if (g_ue4ssReadyEvent)
	{
		SetEvent(g_ue4ssReadyEvent);
		CloseHandle(g_ue4ssReadyEvent);
		g_ue4ssReadyEvent = NULL;
	}

	// Wait for the init thread to finish before tearing down any subsystem.
	// g_pluginsLoadedEvent does not need LoadLibrary to be waited on, so
	// there is no deadlock risk even though the loader lock is held here.
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

	// CRITICAL: Remove the EngineShutdown hook and clear its callbacks FIRST.
	// This prevents the hook from firing with dangling function pointers
	// after plugins have been unloaded.
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
// ---------------------------------------------------------------------------
// Auto-update runs on its own thread in parallel with hook installation and
// the engine-ready wait.  This lets plugin updates download concurrently with
// engine initialization so startup is faster.  MainInitThreadProc joins this
// thread after the engine is ready (gate 2) before loading plugins, ensuring
// all updated DLLs are on disk before LoadAllPlugins scans the directory.
// ---------------------------------------------------------------------------
static DWORD WINAPI AutoUpdateThreadProc(LPVOID)
{
	ModLoaderLogger::RunAutoUpdate();
	return 0;
}

// ---------------------------------------------------------------------------
// Named startup phases -- called in sequence from MainInitThreadProc.
// ---------------------------------------------------------------------------

static void InitSubsystems()
{
	Splash::SetStatus(L"Initializing logger...");
	Splash::SetProgress(0.05f);
	ModLoaderLogger::InitializeLogger();
	ModLoaderLogger::LogMessage(L"======================================");
	ModLoaderLogger::LogMessage(L"  AlienX's Mod Loader Starting");
	ModLoaderLogger::LogMessage(L"======================================");

	ModLoaderLogger::InitializeConfigManager();
	PluginManager::InitializePluginManager();
}

static void InstallHooksPhase()
{
	Splash::SetStatus(L"Installing core game hooks...");
	Splash::SetProgress(0.15f);
	InstallAllHooks();
}

static void WaitForEnginePhase()
{
	// WaitForEngineReady updates splash status internally.
	WaitForEngineReady();
	Splash::SetProgress(0.50f);
}

static void LoadPluginsPhase()
{
	Splash::SetStatus(L"Loading plugin DLLs...");
	Splash::SetProgress(0.50f);
	PluginManager::LoadAllPlugins();
	Splash::SetProgress(0.85f);
}

static void InitPluginsIfReady()
{
	// Bug fix: if GameInstanceInit one-shot latch fired before plugins were
	// loaded (server startup race / 120s timeout scenario), PluginInit was
	// never called.  Detect this and call InitAllLoadedPlugins now.
	if (Hooks::GameInstanceInit::HasFired())
	{
		ModLoaderLogger::LogInfo(L"[dllmain] GameInstanceInit already fired before plugins loaded -- calling InitAllLoadedPlugins now");
		PluginManager::InitAllLoadedPlugins();
	}
	PluginManager::MarkStartupComplete();
}

// ---------------------------------------------------------------------------
// Main initialization thread.
//
// Runs outside the loader lock so WinHTTP (auto-update), LoadLibrary (plugin
// DLLs), and MsgWaitForMultipleObjects (engine-ready wait) can all work.
//
// Two-gate ordering guarantee before LoadAllPlugins:
//   Gate 1: g_engineReadyEvent -- engine is fully initialized (WaitForEnginePhase)
//   Gate 2: g_autoUpdateThread -- all plugin DLLs are downloaded (join thread)
// Both must be satisfied before we scan the Plugins directory.
// ---------------------------------------------------------------------------
static DWORD WINAPI MainInitThreadProc(LPVOID)
{
	// Open the splash here, not in DllMain.  The splash window has no internal
	// message thread -- Show/SetStatus/SetProgress/Close must all be called from
	// the same thread that owns the HWND.
	Splash::Show();
	Splash::SetStatus(L"Starting mod loader...");
	Splash::SetProgress(0.0f);

	if (!VerifyGameVersion())
		return 0;

	LogStartupEnvironment();
	InitSubsystems();

	Hooks::Input::InstallInputProcessor();
	Hooks::WorldBeginPlay::Install();
	Hooks::EngineTick::Install();

	// Kick off auto-update in parallel with hook installation and engine wait.
	// The splash sub-bar will show per-plugin update progress from that thread.
	Splash::SetStatus(L"Checking for updates and installing hooks...");
	g_autoUpdateThread = CreateThread(nullptr, 0, AutoUpdateThreadProc, nullptr, 0, nullptr);
	if (!g_autoUpdateThread)
		LogToFile::Warn("[ModLoader] Failed to create auto-update thread (%lu) -- running synchronously", GetLastError());

	if (g_autoUpdateThread)
	{
		InstallHooksPhase();
		WaitForEnginePhase();

		// Gate 2: ensure auto-update has finished before we scan Plugins dir.
		Splash::SetStatus(L"Waiting for update check to complete...");
		WaitForSingleObject(g_autoUpdateThread, INFINITE);
		CloseHandle(g_autoUpdateThread);
		g_autoUpdateThread = NULL;
	}
	else
	{
		// Fallback: run synchronously if thread creation failed.
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

	// Wait for InitAllLoadedPlugins to complete -- it may fire on the game
	// thread via the GameInstanceInit hook rather than synchronously above.
	// Keep pumping messages so the splash stays responsive.
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

	// Signal DLL_PROCESS_DETACH that init is complete.
	if (g_pluginsLoadedEvent)
		SetEvent(g_pluginsLoadedEvent);

	// Linger briefly so the "Ready." state is visible before closing.
	Splash::Linger(1200);
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

	if (IsWindows10OrGreater()) LogToFile::Info("OS version: Windows 10 or greater");
	else if (IsWindows8Point1OrGreater()) LogToFile::Info("OS version: Windows 8.1");
	else if (IsWindows8OrGreater())       LogToFile::Info("OS version: Windows 8");
	else if (IsWindows7OrGreater())    LogToFile::Info("OS version: Windows 7");
	else if (IsWindowsVistaOrGreater())   LogToFile::Info("OS version: Windows Vista");
	else   LogToFile::Info("OS version: Windows XP or older");

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
	const std::wstring iniPath = GetExeDirPath(L"modloader.ini");

	if (!GetPrivateProfileIntW(L"UE4SS", L"Enabled", 1, iniPath.c_str()))
	{
		LogToFile::Info("UE4SS loading disabled in modloader.ini");
		return;
	}

	wchar_t relPath[MAX_PATH]{};
	GetPrivateProfileStringW(L"UE4SS", L"Path", L"ue4ss\\ue4ss.dll", relPath, MAX_PATH, iniPath.c_str());

	// Resolve the (potentially relative) path against the exe directory.
	const std::wstring fullPath = GetExeDir() + relPath;

	LogToFile::Info("Loading UE4SS from: %ls", fullPath.c_str());

	HMODULE hUE4SS = LoadLibraryW(fullPath.c_str());
	if (hUE4SS)
	{
		LogToFile::Info("UE4SS loaded successfully (handle 0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hUE4SS)));
	}
	else
	{
		LogToFile::Warn("UE4SS failed to load (error %lu): %ls", GetLastError(), fullPath.c_str());
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

		// Refresh the module list so our own symbols are available for stack
		// traces from the very start.
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
			CloseHandle(g_engineReadyEvent); g_engineReadyEvent = NULL;
			DwmapiProxy::Shutdown();
			LogToFile::Shutdown();
			return FALSE;
		}

		// Auto-reset event signalled by the EngineInit detour once its
		// call-stack has fully unwound.  Consumed once by the UE4SS loader thread.
		g_ue4ssReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!g_ue4ssReadyEvent)
		{
			// Non-fatal: the background thread will fall back to a 15s timeout.
			LogToFile::Warn("Failed to create UE4SS-ready event (%lu) -- UE4SS load will use timeout fallback", GetLastError());
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
				LogToFile::Error("FATAL: Failed to create main init thread (%lu)", GetLastError());
				CloseHandle(g_pluginsLoadedEvent); g_pluginsLoadedEvent = NULL;
				CloseHandle(g_engineReadyEvent);   g_engineReadyEvent   = NULL;
				if (g_ue4ssReadyEvent) { CloseHandle(g_ue4ssReadyEvent); g_ue4ssReadyEvent = NULL; }
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

		ShutdownAll();
	}
	break;

	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return TRUE;
}
