#pragma once

#include "plugin_interface.h"

namespace ServerUtilityConfig
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
			"MaxPlayers",
			ConfigValueType::Integer,
			"0",
			"Override the hardcoded max player limit (default game limit is 4). Set to 0 to leave unchanged."
		},
		{
			"PluginSettings",
			"RemoteVulnerabilityPatch",
			ConfigValueType::Boolean,
			"true",
			"Blocks unauthorized /remote/object/call HTTP requests. Only calls targeting the DedicatedServerSettingsComp object path are permitted. Attempts are logged as warnings."
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
				s_config->InitializeFromSchema("ServerUtility", &SCHEMA);
			}
		}

		static bool IsPluginEnabled()
		{
			return s_config ? s_config->ReadBool("ServerUtility", "General", "Enabled", false) : false;
		}

		// Returns the configured max player count.
		// 0 means "don't patch / use game default".
		static int GetMaxPlayers()
		{
			int val = s_config ? s_config->ReadInt("ServerUtility", "PluginSettings", "MaxPlayers", 0) : 0;
			if (val < 0) val = 0;
			if (val > 127) val = 127;
			return val;
		}

		// Returns true if the RemoteVulnerabilityPatch is enabled (default: true).
		static bool GetRemoteVulnerabilityPatch()
		{
			return s_config ? s_config->ReadBool("ServerUtility", "PluginSettings", "RemoteVulnerabilityPatch", true) : true;
		}

	private:
		static IPluginConfig* s_config;
	};
}
