#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "compass/compass.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.2"
#endif

static PluginInfo s_pluginInfo = {
	"Compass",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Client-side HUD compass overlay",
	PLUGIN_INTERFACE_VERSION
};

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		g_self = self;

		LOG_INFO("Compass plugin initializing...");

		CompassConfig::Config::Initialize(self);

		if (!CompassConfig::Config::IsEnabled())
		{
			LOG_WARN("Compass is disabled in config — skipping hook install");
			return true;
		}


		// Resolve StaticLoadObject so textures can be force-loaded without
		// requiring the player to open the map first.
		// Address is pre-scanned by the modloader at startup via hooks->Engine.
		{
			uintptr_t slo = self->hooks->Engine->GetStaticLoadObjectAddress();
			if (slo)
				Compass::SetStaticLoadObject(slo);
			else
				LOG_WARN("StaticLoadObject address not available -- map textures require player to open map first");
		}

		if (!Compass::Install(self->hooks))
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

		Compass::Remove(g_self ? g_self->hooks : nullptr);

		g_self = nullptr;
	}

} // extern "C"
