#include "plugin.h"
#include "plugin_helpers.h"

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

	LOG_INFO("This plugin has been removed - it is safe to delete this dll file");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");

	g_logger = nullptr;
	g_config = nullptr;
	g_scanner = nullptr;
	g_hooks = nullptr;
}
} // extern "C"
