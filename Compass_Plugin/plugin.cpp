#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "compass/compass.h"
#include <compass_patterns.h>

static IPluginLogger*  g_logger  = nullptr;
static IPluginConfig*  g_config  = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks*   g_hooks   = nullptr;

IPluginLogger*  GetLogger()  { return g_logger;  }
IPluginConfig*  GetConfig()  { return g_config;  }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks*   GetHooks()   { return g_hooks;   }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"Compass",
	MODLOADER_BUILD_TAG,
	"",
	"Client-side HUD compass overlay",
	PLUGIN_INTERFACE_VERSION
};

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks)
	{
		g_logger  = logger;
		g_config  = config;
		g_scanner = scanner;
		g_hooks   = hooks;

		LOG_INFO("Compass plugin initializing...");

		CompassConfig::Config::Initialize(config);

		if (!CompassConfig::Config::IsEnabled())
		{
			LOG_WARN("Compass is disabled in config — skipping hook install");
			return true;
		}


		// Resolve StaticLoadObject so textures can be force-loaded without
		// requiring the player to open the map first.
		if (CompassPatterns::StaticLoadObject)
		{
			uintptr_t slo = scanner->FindPatternInMainModule(CompassPatterns::StaticLoadObject);
			if (slo)
				Compass::SetStaticLoadObject(slo);
			else
				LOG_WARN("StaticLoadObject pattern not found — map textures require player to open map first");
		}
		else
		{
			LOG_WARN("StaticLoadObject pattern not set — map textures require player to open map first");
		}

		if (!Compass::Install(scanner, hooks))
		{
			LOG_WARN("Compass hook install failed — compass will not render");
			// Return true so the plugin still loads; compass just won't draw
			return true;
		}

		LOG_INFO("Compass plugin initialized");
		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Compass plugin shutting down...");

		Compass::Remove(g_hooks);

		g_logger  = nullptr;
		g_config  = nullptr;
		g_scanner = nullptr;
		g_hooks   = nullptr;
	}

} // extern "C"
