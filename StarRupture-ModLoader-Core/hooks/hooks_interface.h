#pragma once

#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
	// Get the global hooks interface for plugins
	IPluginHooks* GetPluginHooks();

	// How often IPluginEngineEvents::GetSdkGlobals has been called on this
	// thread. The plugin manager compares it across PluginInit to see whether
	// a plugin took the globals (VersionMap::OffBuildRefusal).
	unsigned SdkGlobalsCallsOnThisThread();

	// Drops every game-event, tick, input, delegate, network and HTTP callback
	// that lives in `module`: a plugin about to be freed, whatever it did not
	// unregister itself. Called by the plugin manager before FreeLibrary.
	void ForgetPluginCallbacks(HMODULE module);
}
