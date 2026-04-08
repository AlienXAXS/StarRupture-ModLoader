#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "mod_core.h"
#include "Windows.h"

// Global plugin self pointer
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

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
	wchar_t path[MAX_PATH] = {0};
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

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;

	LOG_INFO("Plugin initializing...");

	// Initialize config system with schema - creates default config if needed
	KeepTickingConfig::Config::Initialize(self);

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

	ModCore::Initialize(g_self->scanner, g_self->hooks);

	LOG_INFO("Plugin initialized");
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");
	ModCore::Shutdown();
	g_self = nullptr;
}
} // extern "C"
