#pragma once

#include <windows.h>
#include "plugins/plugin_interface.h"  // Use the global interface definition

namespace ModLoaderLogger
{
	// Initialize the config manager
	void InitializeConfigManager();

	// Shutdown the config manager
	void ShutdownConfigManager();

	// Get the config directory path
	const wchar_t* GetConfigDirectory();

	// Get the global config interface (uses the interface from plugin_interface.h)
	IPluginConfig* GetPluginConfig();

	// Returns the ConfigSchema registered by the plugin, or nullptr if none.
	const ConfigSchema* GetPluginSchema(const char* pluginName);

	// Drops the cached schema pointer for a plugin. MUST be called before that
	// plugin's DLL is unloaded.
	//
	// A registered ConfigSchema lives in the plugin's own module: the entry array
	// and every section, key and description string in it are static data inside
	// the DLL, and the manager stores only the pointer. Once FreeLibrary has run,
	// reading any of it is a read from unmapped memory. That survived unnoticed
	// because Windows often leaves the pages readable for a while, which turns a
	// crash into stale text on screen -- arguably the worse outcome, since it looks
	// like the config UI working.
	void ForgetPluginSchema(const char* pluginName);
}
