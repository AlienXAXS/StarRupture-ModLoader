#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "mod_core.h"

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

static PluginInfo s_pluginInfo = {
	"KeepTicking",
	"1.0.0",
	"AlienX",
	"Prevents dedicated server from sleeping when no players are online",
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

	// Initialize config system with schema - creates default config if needed
	KeepTickingConfig::Config::Initialize(config);
	LOG_INFO("Config initialized (PreventServerSleep: %s)", 
		KeepTickingConfig::Config::ShouldPreventServerSleep() ? "true" : "false");

	ModCore::Initialize(scanner, hooks);

	LOG_INFO("Plugin initialized");
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");
	ModCore::Shutdown();

	// Clear interface pointers
	g_logger = nullptr;
	g_config = nullptr;
	g_scanner = nullptr;
	g_hooks = nullptr;
}

} // extern "C"
