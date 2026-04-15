#pragma once

#include "plugins/plugin_interface.h"

namespace ModLoaderLogger
{
	// Get the global hooks interface for plugins
	IPluginHooks* GetPluginHooks();
}
