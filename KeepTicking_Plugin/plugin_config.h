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
		},
		{
			"Hooks",
			"DebugVisibleMode",
			ConfigValueType::Boolean,
			"false",
			"Make the fake player visible for debugging (enables collision and ticking)"
		},
		{
			"Traversal",
			"WaypointsPerTick",
			ConfigValueType::Integer,
			"10",
			"Number of waypoints the fake player teleports through per engine tick (higher = faster traversal)"
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

		static bool IsDebugVisibleModeEnabled()
		{
			return s_config ? s_config->ReadBool("KeepTicking", "Hooks", "DebugVisibleMode", false) : false;
		}

		static int GetWaypointsPerTick()
		{
			int val = s_config ? s_config->ReadInt("KeepTicking", "Traversal", "WaypointsPerTick", 10) : 10;
			if (val < 1) val = 1;
			if (val > 100) val = 100;
			return val;
		}

	private:
		static IPluginConfig* s_config;
	};
}
