#pragma once

#include "plugin_interface.h"

namespace KeepTickingConfig
{
	// Config schema definition
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"Hooks",
			"PreventServerSleep",
			ConfigValueType::Boolean,
			"false",
			"Spawn a fake player to prevent server from sleeping when empty"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Helper class to access config values with type safety
	class Config
	{
	public:
		static void Initialize(IPluginConfig* config)
		{
			s_config = config;
			
			// Initialize config from schema
			if (s_config)
			{
				s_config->InitializeFromSchema("KeepTicking", &SCHEMA);
			}
		}

		static bool ShouldPreventServerSleep()
		{
			return s_config ? s_config->ReadBool("KeepTicking", "Hooks", "PreventServerSleep", false) : false;
		}

	private:
		static IPluginConfig* s_config;
	};
}
