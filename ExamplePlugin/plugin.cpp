#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

// Global plugin interface pointers
static IPluginLogger* g_logger = nullptr;
static IPluginConfig* g_config = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks* g_hooks = nullptr;

// Helper functions to access plugin interfaces
IPluginLogger* GetLogger() { return g_logger; }
IPluginConfig* GetConfig() { return g_config; }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks* GetHooks() { return g_hooks; }

// Plugin metadata
static PluginInfo s_pluginInfo = {
	"ExamplePlugin",
	"1.0.0",
	"Your Name",
	"A minimal example plugin showing the basic structure required by the mod loader",
	PLUGIN_INTERFACE_VERSION
};

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

		// Initialize config system (optional - creates config file with defaults)
		ExamplePluginConfig::Config::Initialize(config);

		// Check if plugin is enabled via config
		if (!ExamplePluginConfig::Config::IsEnabled())
		{
			LOG_WARN("Plugin is disabled in config file");
			return true; // Return true so plugin loads but doesn't activate
		}

		// Example: Read and log config values
		LOG_INFO("Config values:");
		LOG_INFO("  ExampleString: %s", ExamplePluginConfig::Config::GetExampleString());
		LOG_INFO("  ExampleNumber: %d", ExamplePluginConfig::Config::GetExampleNumber());
		LOG_INFO("  ExampleFloat: %.2f", ExamplePluginConfig::Config::GetExampleFloat());

		// Your initialization code would go here
		// Examples:
		// - Find memory patterns with scanner
		// - Install hooks with hooks interface
		// - Register callbacks

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		// Your cleanup code would go here
		// Examples:
		// - Remove installed hooks
		// - Free allocated resources
		// - Unregister callbacks

		// Clear interface pointers
		g_logger = nullptr;
		g_config = nullptr;
		g_scanner = nullptr;
		g_hooks = nullptr;
	}

} // extern "C"
