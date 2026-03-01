#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "LogisticsFragmentFixer.h"

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
static PluginInfo s_pluginInfo = {
	"RailJunctionFixer",
	"1.0.0",
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

static constexpr const char* MASS_ENTITY_CONFIG_WBP_PATTERN =
"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 48 8D 4C 24";

typedef void(__fastcall* MassEntityConfigWBP_t)(void* thisPtr, SDK::UWorld* inWorld);
static MassEntityConfigWBP_t g_originalMassEntityConfigWBP = nullptr;
static HookHandle g_massEntityConfigWBPHookHandle = nullptr;

static void __fastcall Hook_MassEntityConfigWBP(void* thisPtr, SDK::UWorld* inWorld)
{
	std::string worldName = inWorld ? inWorld->GetName() : "(null)";
	LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay fired - this=%p, World=%p '%s'",
		thisPtr, static_cast<void*>(inWorld), worldName.c_str());

	// Apply the hierarchy patch BEFORE the original runs so it is in place
	// before entity archetypes are compiled (BuildTemplate / IsChildOf calls).
	if (RailJunctionFixerConfig::Config::IsEnabled())
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

	g_massEntityConfigWBPHookHandle = g_hooks->InstallHook(
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
		g_hooks->RemoveHook(g_massEntityConfigWBPHookHandle);
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
	if (!RailJunctionFixerConfig::Config::IsEnabled())
		return;

	LOG_INFO("ExperienceLoadComplete: Experience fully loaded - running junction repair");

	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world)
	{
		LOG_ERROR("ExperienceLoadComplete: UWorld::GetWorld() returned null - cannot repair junctions");
		return;
	}

	std::string worldName = world->GetName();
	LOG_INFO("ExperienceLoadComplete: Current world is '%s' at %p", worldName.c_str(), static_cast<void*>(world));
}

// ----------------------------------------------------------------
// Save-loaded callback
// ----------------------------------------------------------------
// Called by the mod loader when UCrMassSaveSubsystem::OnSaveLoaded
// fires - the save is fully loaded. We use this to signal socket
// entities so that stale FMassEntityHandle values are rebuilt.
// ----------------------------------------------------------------

static void OnSaveLoaded()
{
	if (!RailJunctionFixerConfig::Config::IsEnabled())
		return;

	LOG_INFO("SaveLoaded: Save finished loading - signaling socket entities for re-initialization");
	RailJunctionFixer::LogisticsFragmentFixer::SignalSocketEntities();
}

// Engine init callback - called after FEngineLoop::Init returns.
// NOTE: UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay fires INSIDE
// FEngineLoop::Init, so the WBP hook is installed from PluginInit instead.
static void OnEngineInit()
{
	LOG_INFO("Engine initialized");

	// Register for save-loaded callback to signal socket entities after a save loads
	if (RailJunctionFixerConfig::Config::IsEnabled())
	{
		if (g_hooks && g_hooks->RegisterSaveLoadedCallback)
		{
			g_hooks->RegisterSaveLoadedCallback(OnSaveLoaded);
			LOG_INFO("Registered for save-loaded callback (socket entity signaling)");
		}
		else
		{
			LOG_WARN("RegisterSaveLoadedCallback not available - socket re-init after save load will NOT run");
		}
	}
}

// Engine shutdown callback - called before UObject system tears down
// This is the correct place to restore UStruct patches so the engine
// doesn't encounter our VirtualAlloc'd memory during its own teardown.
static void OnEngineShutdown()
{
	LOG_INFO("Engine shutting down - cleaning up...");

	// Unregister save-loaded callback
	if (g_hooks && g_hooks->UnregisterSaveLoadedCallback)
	{
		g_hooks->UnregisterSaveLoadedCallback(OnSaveLoaded);
	}

	// Unregister experience-load-complete callback
	if (g_hooks && g_hooks->UnregisterExperienceLoadCompleteCallback)
	{
		g_hooks->UnregisterExperienceLoadCompleteCallback(OnExperienceLoadComplete);
	}

	RailJunctionFixer::LogisticsFragmentFixer::Shutdown();

	// Remove the mass entity config loader subsystem hook
	RemoveMassEntityConfigWBPHook();
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks)
	{
		// Store plugin interface pointers
		g_logger = logger;
		g_config = config;
		g_scanner = scanner;
		g_hooks = hooks;

		LOG_INFO("Plugin initializing...");

		// Initialize config system with schema - creates default config if needed
		RailJunctionFixerConfig::Config::Initialize(config);
		LOG_INFO("Config initialized (Enabled: %s)",
			RailJunctionFixerConfig::Config::IsEnabled() ? "true" : "false");

		// Install the UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook NOW,
		// before FEngineLoop::Init runs. The subsystem fires OnWorldBeginPlay inside
		// FEngineLoop::Init, so this must be in place before the engine starts up.
		if (InstallMassEntityConfigWBPHook())
		{
			LOG_INFO("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook installed");
		}
		else
		{
			LOG_ERROR("UCrMassEntityConfigLoaderSubsystem::OnWorldBeginPlay hook FAILED - hierarchy patch will not be applied");
		}

		// Register for engine init callback (for AActor::BeginPlay diagnostic and
		// ExperienceLoadComplete registration, which are safe to install post-engine-init)
		if (!hooks->RegisterEngineInitCallback)
		{
			LOG_ERROR("RegisterEngineInitCallback not available - loader version mismatch?");
			return false;
		}

		hooks->RegisterEngineInitCallback(OnEngineInit);
		LOG_INFO("Registered for engine init callback");

		if (hooks->RegisterEngineShutdownCallback)
		{
			hooks->RegisterEngineShutdownCallback(OnEngineShutdown);
			LOG_INFO("Registered for engine shutdown callback");
		}
		else
		{
			LOG_WARN("RegisterEngineShutdownCallback not available - UStruct restore will not run before engine teardown");
		}

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		// NOTE: Do NOT touch engine callback lists or engine-owned memory here.
		// PluginShutdown is called from DLL_PROCESS_DETACH only if lpReserved==nullptr
		// (explicit FreeLibrary), but in practice server shutdown always goes through
		// ExitProcess (lpReserved!=nullptr) so this function is never called.
		//
		// UStruct restoration is handled in OnEngineShutdown() which fires via the
		// FEngineLoop::Exit hook - before the UObject system tears down.

		g_logger = nullptr;
		g_config = nullptr;
		g_scanner = nullptr;
		g_hooks = nullptr;
	}

} // extern "C"
