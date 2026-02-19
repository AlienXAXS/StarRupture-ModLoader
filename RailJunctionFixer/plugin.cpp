#include "plugin.h"
#include "plugin_helpers.h"
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
	"Fixes rail junction issues in the game for 3x and 5x rails",
	PLUGIN_INTERFACE_VERSION
};

// Helper functions to access plugin interfaces
IPluginLogger* GetLogger() { return g_logger; }
IPluginConfig* GetConfig() { return g_config; }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks* GetHooks() { return g_hooks; }

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

		// Initialize the LogisticsFragmentFixer
		if (!RailJunctionFixer::LogisticsFragmentFixer::Initialize())
		{
			LOG_ERROR("Failed to initialize LogisticsFragmentFixer");
			return false;
		}

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		// Shutdown the LogisticsFragmentFixer
		RailJunctionFixer::LogisticsFragmentFixer::Shutdown();

		g_logger = nullptr;
		g_config = nullptr;
		g_scanner = nullptr;
		g_hooks = nullptr;
	}

} // extern "C"
