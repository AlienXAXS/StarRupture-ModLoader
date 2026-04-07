#pragma once

#include "plugin_interface.h"

namespace NetChannelTestConfig
{
	static constexpr ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the plugin."
		},
		{
			"General",
			"SendIntervalTicks",
			ConfigValueType::Integer,
			"300",
			"Number of engine ticks between each TestPacket broadcast from the server."
		},
	};

	static constexpr ConfigSchema SCHEMA = {
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
				s_config->InitializeFromSchema("NetChannelTest", &SCHEMA);
		}

		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("NetChannelTest", "General", "Enabled", true) : true;
		}

		static int GetSendIntervalTicks()
		{
			int val = s_config ? s_config->ReadInt("NetChannelTest", "General", "SendIntervalTicks", 300) : 300;
			if (val < 1) val = 1;
			return val;
		}

	private:
		static IPluginConfig* s_config;
	};
}
