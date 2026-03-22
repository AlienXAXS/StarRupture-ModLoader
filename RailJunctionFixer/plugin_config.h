#pragma once

#include "plugin_interface.h"

namespace RailJunctionFixerConfig
{
	// Config schema definition
	static constexpr ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"false",
			"Enable or disable the plugin."
		},
		{
			"PluginSettings",
			"FixRailJunctions",
			ConfigValueType::Boolean,
			"false",
			"Enable the rail junction fixer."
		}
	};

	static constexpr ConfigSchema SCHEMA = {
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

		static bool IsPluginEnabled()
		{
			return s_config ? s_config->ReadBool("RailJunctionFixer", "General", "Enabled", false) : false;
		}

		static bool IsRailFixerEnabled()
		{
			return s_config
				       ? s_config->ReadBool("RailJunctionFixer", "PluginSettings", "FixRailJunctions", false)
				       : false;
		}

	private:
		static IPluginConfig* s_config;
	};
}
