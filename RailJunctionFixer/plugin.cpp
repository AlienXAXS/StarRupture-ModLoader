#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "LogisticsFragmentFixer.h"
#include "rail_scanner.h"

#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"

#include <string>
#include <string_view>

// Global plugin interface pointers
static IPluginLogger* g_logger = nullptr;
static IPluginConfig* g_config = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks* g_hooks = nullptr;

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.3"
#endif

static PluginInfo s_pluginInfo = {
	"RailJunctionFixer",
	MODLOADER_BUILD_TAG,
	"Wilhelm & AlienX",
	"Fixes rail junction save/load issues by patching FCrLogisticsSocketsFragment inheritance",
	PLUGIN_INTERFACE_VERSION
};

// Helper functions to access plugin interfaces
IPluginLogger* GetLogger() { return g_logger; }
IPluginConfig* GetConfig() { return g_config; }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks* GetHooks() { return g_hooks; }

// ----------------------------------------------------------------
// UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook
// ----------------------------------------------------------------
// Signature from IDA:
//   void __fastcall UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay(
//    UCrMassEntityConfigLoaderSubsystem *this, UWorld *InWorld)
//
// Installed from PluginInit (NOT OnEngineInit) because this subsystem's
// OnWorldBeginPlay fires INSIDE FEngineLoop::Init before it returns, so
// the hook must be in place before FEngineLoop::Init runs.
//
// The hierarchy patch runs BEFORE calling the original so that
// FCrLogisticsSocketsFragment's IsChildOf chain is correct when the
// subsystem compiles entity archetypes (BuildTemplate calls).
// ----------------------------------------------------------------

static constexpr auto MASS_ENTITY_CONFIG_WBP_PATTERN =
	"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 48 8D 4C 24";

using MassEntityConfigWBP_t = void(__fastcall*)(void* thisPtr, SDK::UWorld* inWorld);
static MassEntityConfigWBP_t g_originalMassEntityConfigWBP = nullptr;
static HookHandle g_massEntityConfigWBPHookHandle = nullptr;

static void __fastcall Hook_MassEntityConfigWBP(void* thisPtr, SDK::UWorld* inWorld)
{
	std::string worldName = inWorld ? inWorld->GetName() : "(null)";
	LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay fired - this=%p, World=%p '%s'",
	         thisPtr, static_cast<void*>(inWorld), worldName.c_str());

	// Apply the hierarchy patch BEFORE the original runs so it is in place
	// before entity archetypes are compiled (BuildTemplate / IsChildOf calls).
	if (RailJunctionFixerConfig::Config::IsRailFixerEnabled())
	{
		if (!RailJunctionFixer::LogisticsFragmentFixer::Initialize())
			LOG_ERROR("LogisticsFragmentFixer: Failed to apply hierarchy patch");
	}

	// Call original
	if (g_originalMassEntityConfigWBP)
	{
		g_originalMassEntityConfigWBP(thisPtr, inWorld);
	}
}

static bool InstallMassEntityConfigWBPHook()
{
	if (!g_scanner || !g_hooks)
	{
		LOG_ERROR("MassEntityConfigWBP hook: Scanner or Hooks interface not available");
		return false;
	}

	LOG_INFO("Scanning for UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay...");
	uintptr_t addr = g_scanner->FindPatternInMainModule(MASS_ENTITY_CONFIG_WBP_PATTERN);

	if (!addr)
	{
		LOG_ERROR("MassEntityConfigWBP hook: Pattern not found!");
		return false;
	}

	HMODULE mainModule = GetModuleHandleW(nullptr);
	auto base = reinterpret_cast<uintptr_t>(mainModule);
	LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay found at 0x%llX (base+0x%llX)",
	         static_cast<unsigned long long>(addr),
	         static_cast<unsigned long long>(addr - base));

	g_massEntityConfigWBPHookHandle = g_hooks->Hooks->Install(
		addr,
		reinterpret_cast<void*>(&Hook_MassEntityConfigWBP),
		reinterpret_cast<void**>(&g_originalMassEntityConfigWBP));

	if (!g_massEntityConfigWBPHookHandle)
	{
		LOG_ERROR("MassEntityConfigWBP hook: InstallHook failed!");
		return false;
	}

	LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook installed successfully");
	return true;
}

static void RemoveMassEntityConfigWBPHook()
{
	if (g_massEntityConfigWBPHookHandle && g_hooks)
	{
		LOG_INFO("Removing UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook...");
		g_hooks->Hooks->Remove(g_massEntityConfigWBPHookHandle);
		g_massEntityConfigWBPHookHandle = nullptr;
		g_originalMassEntityConfigWBP = nullptr;
		LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook removed");
	}
}

// ----------------------------------------------------------------
// Experience-load-complete callback
// ----------------------------------------------------------------
// Called by the mod loader when UCrExperienceManagerComponent::
// OnExperienceLoadComplete fires - this is significantly later than
// OnSaveLoaded and guarantees the map/gameplay experience is fully
// ready with all actors (including Mass Entity BP actors) spawned.
// ----------------------------------------------------------------

static void OnExperienceLoadComplete()
{
	if (!RailJunctionFixerConfig::Config::IsRailFixerEnabled())
		return;

	LOG_INFO("ExperienceLoadComplete: Experience fully loaded - running junction repair");

	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world)
	{
		LOG_ERROR("ExperienceLoadComplete: UWorld::GetWorld() returned null - cannot repair junctions");
		return;
	}

	std::string worldName = world->GetName();
	LOG_DEBUG("ExperienceLoadComplete: Current world is '%s' at %p", worldName.c_str(), static_cast<void*>(world));

	if (worldName != "ChimeraMain") return;

	LOG_DEBUG("SaveLoaded: Save finished loading - signaling socket entities for re-initialization");
	RailJunctionFixer::LogisticsFragmentFixer::SignalSocketEntities();
	RailJunctionFixer::RailScanner::ScanRailSocketState(world);
}

// Engine init callback - called after FEngineLoop::Init returns.
// NOTE: UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay fires INSIDE
// FEngineLoop::Init, so the WBP hook is installed from PluginInit instead.
static void OnEngineInit()
{
	LOG_INFO("Engine initialized");

	// Register for save-loaded callback to signal socket entities after a save loads
	if (RailJunctionFixerConfig::Config::IsRailFixerEnabled())
	{
		// Register for experience-load-complete callback to run the rail scanner.
		// This fires after all Mass Entity BP actors are fully spawned, which is
		// later than OnSaveLoaded -- giving all actor data time to settle.
		if (g_hooks && g_hooks->World)
		{
			g_hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadComplete);
			LOG_INFO("Registered for experience-load-complete callback (rail socket scan)");
		}
		else
		{
			LOG_WARN("World sub-interface not available - rail socket scan will NOT run");
		}

		// Register for actor begin-play callback to TRACE log every spawned actor.
		// This is installed after engine init so the hook targets live engine code.
		if (g_hooks && g_hooks->Actors)
		{
			g_hooks->Actors->RegisterOnActorBeginPlay(RailJunctionFixer::RailScanner::OnActorBeginPlay);
			LOG_INFO("Registered for actor-begin-play callback (TRACE diagnostics)");
		}
		else
		{
			LOG_WARN("Actors sub-interface not available - actor spawn diagnostics will NOT run");
		}
	}
}

// Shared cleanup — called from both OnEngineShutdown and PluginShutdown.
// Guards against double-calls via g_hooks nullptr check.
static void DoFullCleanup()
{
	// Remove the low-level WBP hook first so no further calls into our code
	// can happen while we unwind everything else.
	RemoveMassEntityConfigWBPHook();

	if (g_hooks)
	{
		if (g_hooks->World)
			g_hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadComplete);

		if (g_hooks->Actors)
			g_hooks->Actors->UnregisterOnActorBeginPlay(RailJunctionFixer::RailScanner::OnActorBeginPlay);
	}

	RailJunctionFixer::LogisticsFragmentFixer::Shutdown();
}

// Engine shutdown callback - called before UObject system tears down.
static void OnEngineShutdown()
{
	LOG_INFO("Engine shutting down - cleaning up...");
	DoFullCleanup();
}

extern "C" {
__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner,
                                      IPluginHooks* hooks)
{
	// Store plugin interface pointers
	g_logger = logger;
	g_config = config;
	g_scanner = scanner;
	g_hooks = hooks;

	LOG_INFO("Plugin initializing...");

	// Initialize config system with schema - creates default config if needed
	RailJunctionFixerConfig::Config::Initialize(config);

	if (!RailJunctionFixerConfig::Config::IsPluginEnabled())
	{
		LOG_INFO("Plugin is disabled in config - skipping initialization");
		return true;
	}

	// Install the UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook NOW,
	// before FEngineLoop::Init runs. The subsystem fires OnWorldBeginPlay inside
	// FEngineLoop::Init, so this must be in place before the engine starts up.
	if (InstallMassEntityConfigWBPHook())
	{
		LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook installed");
	}
	else
	{
		LOG_ERROR(
			"UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook FAILED - hierarchy patch will not be applied");
	}


	hooks->Engine->RegisterOnInit(OnEngineInit);
	LOG_INFO("Registered for engine init callback");

	hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
	LOG_INFO("Registered for engine shutdown callback");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");

	// PluginShutdown is now called on explicit hot-unload (FreeLibrary from the
	// mod loader UI) as well as on process exit. We must do a full teardown here
	// to ensure no code or callbacks in this DLL remain reachable after unload.

	// Unregister engine-level callbacks so they don't fire into unloaded memory.
	if (g_hooks && g_hooks->Engine)
	{
		g_hooks->Engine->UnregisterOnInit(OnEngineInit);
		g_hooks->Engine->UnregisterOnShutdown(OnEngineShutdown);
	}

	// Remove the WBP hook, unregister world/actor callbacks, restore structs.
	DoFullCleanup();

	g_logger = nullptr;
	g_config = nullptr;
	g_scanner = nullptr;
	g_hooks = nullptr;
}
} // extern "C"
