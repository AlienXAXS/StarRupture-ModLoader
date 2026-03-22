#pragma once

#include "plugin_interface.h"

namespace KeepTickingConfig
{
	// Config schema definition
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"false",
			"Enable or disable the plugin."
		},
		{
			"PluginSettings",
			"PreventServerSleep",
			ConfigValueType::Boolean,
			"false",
			"Spawn a fake player to prevent server from sleeping when empty"
		},
		{
			"PluginSettings",
			"DebugVisibleMode",
			ConfigValueType::Boolean,
			"false",
			"Make the fake player visible for debugging (enables collision and ticking)"
		},
		{
			"PluginSettings",
			"WaypointsPerTick",
			ConfigValueType::Integer,
			"10",
			"Number of waypoints the fake player teleports through per engine tick (higher = faster traversal)"
		},
		{
			"PluginSettings",
			"DisableTraversal",
			ConfigValueType::Boolean,
			"false",
			"Disable the fake player map traversal teleportation entirely (fake player still spawns to keep server alive)"
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

		static bool IsPluginEnabled()
		{
			return s_config ? s_config->ReadBool("KeepTicking", "General", "Enabled", false) : false;
		}

		static bool ShouldPreventServerSleep()
		{
			return s_config ? s_config->ReadBool("KeepTicking", "PluginSettings", "PreventServerSleep", false) : false;
		}

		static bool IsDebugVisibleModeEnabled()
		{
			return s_config ? s_config->ReadBool("KeepTicking", "PluginSettings", "DebugVisibleMode", false) : false;
		}

		static bool IsTraversalDisabled()
		{
			return s_config ? s_config->ReadBool("KeepTicking", "PluginSettings", "DisableTraversal", false) : false;
		}

		static int GetWaypointsPerTick()
		{
			int val = s_config ? s_config->ReadInt("KeepTicking", "PluginSettings", "WaypointsPerTick", 10) : 10;
			if (val < 1) val = 1;
			if (val > 100) val = 100;
			return val;
		}

	private:
		static IPluginConfig* s_config;
	};
}
