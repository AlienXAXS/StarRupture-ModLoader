#pragma once

#include "plugin_interface.h"

namespace RailJunctionFixerConfig
{
	// Config schema definition
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"false",
			"Enable the rail junction fixer. WARNING: Experimental! Only enable if you experience rail junction save/load issues."
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
				s_config->InitializeFromSchema("RailJunctionFixer", &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("RailJunctionFixer", "General", "Enabled", false) : false;
		}

	private:
		static IPluginConfig* s_config;
	};
}
