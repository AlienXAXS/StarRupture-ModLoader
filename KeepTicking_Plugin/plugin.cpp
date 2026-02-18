#include "plugin.h"
#include "mod_core.h"
#include "plugin_logger.h"

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

__declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* /*config*/, IPluginScanner* scanner, IPluginHooks* hooks)
{
	PluginLogger::Initialize(logger);
	PluginLogger::Info("KeepTicking plugin initializing...");

	ModCore::Initialize(scanner, hooks);

	PluginLogger::Info("KeepTicking plugin initialized");
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	PluginLogger::Info("KeepTicking plugin shutting down...");
	ModCore::Shutdown();
}

} // extern "C"
