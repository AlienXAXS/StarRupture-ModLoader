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
		},
		{
			"General",
			"AutoFixExistingJunctions",
			ConfigValueType::Boolean,
			"false",
			"On world load, scan for pre-existing 3x/5x junctions with missing socket connections and re-trigger socket registration on their rails."
		},
		{
			"General",
			"DryRun",
			ConfigValueType::Boolean,
			"true",
			"When true, the junction repair scan will only LOG what it would fix without actually calling OnSplineReady. Set to false once you are satisfied the detection looks correct."
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

		static bool AutoFixExistingJunctions()
		{
			return s_config ? s_config->ReadBool("RailJunctionFixer", "General", "AutoFixExistingJunctions", false) : false;
		}

		static bool DryRun()
		{
			return s_config ? s_config->ReadBool("RailJunctionFixer", "General", "DryRun", true) : true;
		}

	private:
		static IPluginConfig* s_config;
	};
}
