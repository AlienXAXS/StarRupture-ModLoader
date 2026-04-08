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
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;
			if (s_self)
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		static int GetSendIntervalTicks()
		{
			int val = s_self ? s_self->config->ReadInt(s_self, "General", "SendIntervalTicks", 300) : 300;
			if (val < 1) val = 1;
			return val;
		}

	private:
		static IPluginSelf* s_self;
	};
}
