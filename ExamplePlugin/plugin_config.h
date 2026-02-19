#pragma once

#include "plugin_interface.h"

namespace ExamplePluginConfig
{
	// Define config schema with a few simple examples
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the example plugin"
		},
		{
			"General",
			"ExampleString",
			ConfigValueType::String,
			"Hello World",
			"An example string value"
		},
		{
			"Settings",
			"ExampleNumber",
			ConfigValueType::Integer,
			"42",
			"An example integer value"
		},
		{
			"Settings",
			"ExampleFloat",
			ConfigValueType::Float,
			"3.14",
			"An example float value"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginConfig* config)
		{
			s_config = config;
			
			// Initialize config from schema - creates file with defaults if missing
			if (s_config)
			{
				s_config->InitializeFromSchema("ExamplePlugin", &SCHEMA);
			}
		}

		// Example accessors for each config value
		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("ExamplePlugin", "General", "Enabled", true) : true;
		}

		static const char* GetExampleString()
		{
			static char buffer[256];
			if (s_config && s_config->ReadString("ExamplePlugin", "General", "ExampleString", buffer, sizeof(buffer), "Hello World"))
			{
				return buffer;
			}
			return "Hello World";
		}

		static int GetExampleNumber()
		{
			return s_config ? s_config->ReadInt("ExamplePlugin", "Settings", "ExampleNumber", 42) : 42;
		}

		static float GetExampleFloat()
		{
			return s_config ? s_config->ReadFloat("ExamplePlugin", "Settings", "ExampleFloat", 3.14f) : 3.14f;
		}

	private:
		static IPluginConfig* s_config;
	};
}
