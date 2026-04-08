#include "plugin.h"
#include "plugin_helpers.h"

// Global plugin self pointer
static IPluginSelf* g_self = nullptr;

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

IPluginSelf* GetSelf() { return g_self; }

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

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;

	LOG_INFO("This plugin has been removed - it is safe to delete this dll file");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");
	g_self = nullptr;
}
} // extern "C"
