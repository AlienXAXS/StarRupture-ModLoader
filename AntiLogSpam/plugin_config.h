#pragma once

#include "plugin_interface.h"

namespace AntiLogSpamConfig
{
	// Config schema definition
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"PatchMiningGunLogSpam",
			ConfigValueType::Boolean,
			"false",
			"Patch UObjectBase::IsValidLowLevelFast to silently suppress null-pointer log spam from the mining gun."
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

			if (s_config)
			{
				s_config->InitializeFromSchema("AntiLogSpam", &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("AntiLogSpam", "General", "PatchMiningGunLogSpam", true) : false;
		}

	private:
		static IPluginConfig* s_config;
	};
}
