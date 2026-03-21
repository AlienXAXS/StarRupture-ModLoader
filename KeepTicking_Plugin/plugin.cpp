#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "mod_core.h"
#include "Windows.h"

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

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.5"
#endif

static PluginInfo s_pluginInfo = {
	"KeepTicking",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Prevents dedicated server from sleeping when no players are online",
	PLUGIN_INTERFACE_VERSION
};

static bool IsServerBinary()
{
	wchar_t path[MAX_PATH] = { 0 };
	if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
	{
		// If desired, log failure via GetLogger(); keep simple here.
		return false;
	}

	// Extract filename part
	wchar_t* filename = wcsrchr(path, L'\\');
	if (!filename)
		filename = wcsrchr(path, L'/');

	const wchar_t* exeName = filename ? (filename + 1) : path;

	// Case-insensitive compare
	return _wcsicmp(exeName, L"StarRuptureServerEOS-Win64-Shipping.exe") == 0;
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
	KeepTickingConfig::Config::Initialize(config);

	if (!KeepTickingConfig::Config::IsPluginEnabled())
	{
		LOG_INFO("Plugin is disabled in config - skipping initialization");	
		return true;
	}

	if (!IsServerBinary())
	{
		LOG_WARN("This plugin is intended for dedicated server use only - skipping initialization");
		return true;
	}

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
