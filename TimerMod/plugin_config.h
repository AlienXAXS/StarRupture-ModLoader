#pragma once

#include "plugin_interface.h"

namespace RuptureTimerConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		// --- General ---
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the RuptureTimer plugin"
		},
		// --- Export (StreamDeck / external tools) ---
		{
			"Export",
			"WriteJsonFile",
			ConfigValueType::Boolean,
			"true",
			"Write rupture timer state to a JSON file for external tools (e.g. StreamDeck)"
		},
		{
			"Export",
			"JsonFilePath",
			ConfigValueType::String,
			"Plugins/data/rupture_timer.json",
			"Path to the JSON output file (relative to game directory)"
		},
		{
			"Export",
			"UpdateIntervalSeconds",
			ConfigValueType::Float,
			"1.0",
			"How often (in seconds) to update the JSON file"
		},
		{
			"Export",
			"ExtendedPhaseTimers",
			ConfigValueType::Boolean,
			"false",
			"Add per-phase timing breakdown to JSON: warning_remaining_sec, burning_remaining_sec, cooling_remaining_sec, stabilizing_remaining_sec, stable_remaining_sec"
		},
		// --- HUD (in-game overlay) ---
		{
			"HUD",
			"ShowOverlay",
			ConfigValueType::Boolean,
			"false",
			"Show rupture timer as an in-game text overlay"
		},
		{
			"HUD",
			"Position",
			ConfigValueType::String,
			"LowerLeft",
			"Overlay anchor position: LowerLeft, MidLeft, TopLeft, TopMid, TopRight, MidRight, LowerRight"
		},
		{
			"HUD",
			"Scale",
			ConfigValueType::Float,
			"1.0",
			"Text scale multiplier (1.0 = default engine font size)"
		},
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
				s_config->InitializeFromSchema("RuptureTimer", &SCHEMA);
		}

		// --- General ---
		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("RuptureTimer", "General", "Enabled", true) : true;
		}

		// --- Export ---
		static bool ShouldWriteJsonFile()
		{
			return s_config ? s_config->ReadBool("RuptureTimer", "Export", "WriteJsonFile", true) : true;
		}

		static const char* GetJsonFilePath()
		{
			static char buffer[512];
			if (s_config && s_config->ReadString("RuptureTimer", "Export", "JsonFilePath", buffer, sizeof(buffer), "Plugins/data/rupture_timer.json"))
				return buffer;
			return "Plugins/data/rupture_timer.json";
		}

		static float GetUpdateIntervalSeconds()
		{
			return s_config ? s_config->ReadFloat("RuptureTimer", "Export", "UpdateIntervalSeconds", 1.0f) : 1.0f;
		}

		static bool ShouldWriteExtendedPhaseTimers()
		{
			return s_config ? s_config->ReadBool("RuptureTimer", "Export", "ExtendedPhaseTimers", false) : false;
		}

		// --- HUD ---
		static bool ShouldShowOverlay()
		{
			return s_config ? s_config->ReadBool("RuptureTimer", "HUD", "ShowOverlay", false) : false;
		}

		static const char* GetOverlayPosition()
		{
			static char buffer[64];
			if (s_config && s_config->ReadString("RuptureTimer", "HUD", "Position", buffer, sizeof(buffer), "LowerLeft"))
				return buffer;
			return "LowerLeft";
		}

		static float GetOverlayScale()
		{
			float scale = s_config ? s_config->ReadFloat("RuptureTimer", "HUD", "Scale", 1.0f) : 1.0f;
			if (scale < 0.25f) scale = 0.25f;
			if (scale > 5.0f)  scale = 5.0f;
			return scale;
		}

	private:
		static IPluginConfig* s_config;
	};
}
