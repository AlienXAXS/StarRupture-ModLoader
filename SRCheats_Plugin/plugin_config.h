#pragma once

#include "plugin_interface.h"

namespace SRCheatsConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable SRCheats"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	class Config
	{
	public:
		static void Initialize(IPluginConfig* config)
		{
			s_config = config;
			if (s_config)
			{
				s_config->InitializeFromSchema("SRCheats", &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("SRCheats", "General", "Enabled", true) : true;
		}

	private:
		static IPluginConfig* s_config;
	};
}
