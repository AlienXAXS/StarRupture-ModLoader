#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "LogisticsFragmentFixer.h"

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

// Engine init callback - called when UE5 engine finishes initialization
static void OnEngineInit()
{
	LOG_INFO("Engine initialized - checking if fix should be applied...");

	// Check if plugin is enabled via config
	if (!RailJunctionFixerConfig::Config::IsEnabled())
	{
		LOG_WARN("Plugin is DISABLED in config file");
		LOG_INFO("To enable: Set 'Enabled' to 'true' in alienx_mods/configs/RailJunctionFixer.json");
		return;
	}

	LOG_INFO("Plugin is enabled - applying logistics fragment fix...");

	// Initialize the LogisticsFragmentFixer now that engine is ready
	if (!RailJunctionFixer::LogisticsFragmentFixer::Initialize())
	{
		LOG_ERROR("Failed to initialize LogisticsFragmentFixer");
		return;
	}

	LOG_INFO("Logistics fragment fix applied successfully");
}

// Engine shutdown callback - called before UObject system tears down
// This is the correct place to restore UStruct patches so the engine
// doesn't encounter our VirtualAlloc'd memory during its own teardown.
static void OnEngineShutdown()
{
	LOG_INFO("Engine shutting down - restoring UStruct hierarchy...");
	RailJunctionFixer::LogisticsFragmentFixer::Shutdown();
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

		// Register for engine init callback - we need to wait until engine is ready
		// before patching UStruct metadata
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

		if (RailJunctionFixerConfig::Config::IsEnabled())
		{
			LOG_INFO("Fix will be applied when engine is ready");
		}
		else
		{
			LOG_INFO("Fix is DISABLED - plugin will not modify game memory");
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
