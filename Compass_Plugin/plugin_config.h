#pragma once

#include "plugin_interface.h"

namespace CompassConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		// ----- General -----
		{ "General", "Enabled",                  ConfigValueType::Boolean, "true",    "Enable or disable the compass overlay" },
		{ "General", "TextOnly",                 ConfigValueType::Boolean, "true",   "Force text-only mode — never draw icon textures (useful for debugging)" },

		// ----- Compass bar -----
		{ "Compass", "Scale",                    ConfigValueType::Float,   "1.2",     "Compass size multiplier" },
		{ "Compass", "PosY",                     ConfigValueType::Float,   "60.0",    "Pixels from top of screen to the compass line" },
		{ "Compass", "WidthFraction",            ConfigValueType::Float,   "0.50",    "Half-width as fraction of screen width (0.20 = 40% total)" },
		{ "Compass", "EntityScanInterval",       ConfigValueType::Integer, "90",      "Frames between entity scans (90 = ~1.5s at 60fps)" },

		// ----- Players -----
		{ "Players", "Enabled",                  ConfigValueType::Boolean, "true",    "Show other player markers on the compass" },
		{ "Players", "Distance",                 ConfigValueType::Float,   "0.0",     "Max render distance in UU (0 = unlimited)" },

		// ----- Base Cores -----
		{ "BaseCores", "Enabled",                ConfigValueType::Boolean, "true",    "Show base core markers on the compass" },
		{ "BaseCores", "Distance",               ConfigValueType::Float,   "50000.0",     "Max render distance in UU (0 = unlimited)" },

		// ----- Map Markers -----
		{ "Markers", "Enabled",                  ConfigValueType::Boolean, "true",    "Master toggle for all POI markers" },
		{ "Markers", "ShowAntena",               ConfigValueType::Boolean, "true",    "Show antenna POI markers" },
		{ "Markers", "AntenaDistance",           ConfigValueType::Float,   "10000.0", "Antenna max render distance in UU (0 = unlimited)" },
		{ "Markers", "ShowAbandonedBase",        ConfigValueType::Boolean, "true",    "Show abandoned base POI markers" },
		{ "Markers", "AbandonedBaseDistance",    ConfigValueType::Float,   "10000.0", "Abandoned base max render distance in UU (0 = unlimited)" },
		{ "Markers", "ShowCave",                 ConfigValueType::Boolean, "true",    "Show cave POI markers" },
		{ "Markers", "CaveDistance",             ConfigValueType::Float,   "10000.0", "Cave max render distance in UU (0 = unlimited)" },
		{ "Markers", "ShowObelisk",              ConfigValueType::Boolean, "true",    "Show obelisk POI markers" },
		{ "Markers", "ObeliskDistance",          ConfigValueType::Float,   "10000.0", "Obelisk max render distance in UU (0 = unlimited)" },

		// ----- Dead Bodies -----
		{ "Bodies", "Enabled",                   ConfigValueType::Boolean, "true",    "Show dead body markers on the compass" },
		{ "Bodies", "Distance",                  ConfigValueType::Float,   "10000.0", "Max render distance in UU (0 = unlimited)" },

		// ----- Custom Pins -----
		{ "CustomPins", "Enabled",               ConfigValueType::Boolean, "true",    "Show player-placed custom map pins on the compass" },
		{ "CustomPins", "Distance",              ConfigValueType::Float,   "0.0",     "Max render distance in UU (0 = unlimited)" },
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	struct EntitySettings
	{
		bool  enabled;
		float distance; // UU; 0 = unlimited
	};

	struct MarkerSettings
	{
		bool  enabled;
		bool  showAntena;
		float antenaDistance;
		bool  showAbandonedBase;
		float abandonedBaseDistance;
		bool  showCave;
		float caveDistance;
		bool  showObelisk;
		float obeliskDistance;
	};

	class Config
	{
	public:
		static void Initialize(IPluginConfig* config)
		{
			s_config = config;
			if (s_config)
				s_config->InitializeFromSchema("Compass", &SCHEMA);
		}

		static bool IsEnabled()
		{
			return s_config ? s_config->ReadBool("Compass", "General", "Enabled", true) : true;
		}

		static bool IsTextOnly()
		{
			return s_config ? s_config->ReadBool("Compass", "General", "TextOnly", false) : false;
		}

		// ----- Compass bar -----
		static float GetScale()
		{
			return s_config ? s_config->ReadFloat("Compass", "Compass", "Scale", 3.0f) : 3.0f;
		}

		static float GetPosY()
		{
			return s_config ? s_config->ReadFloat("Compass", "Compass", "PosY", 60.0f) : 60.0f;
		}

		static float GetWidthFraction()
		{
			return s_config ? s_config->ReadFloat("Compass", "Compass", "WidthFraction", 0.20f) : 0.20f;
		}

		static int GetEntityScanInterval()
		{
			return s_config ? s_config->ReadInt("Compass", "Compass", "EntityScanInterval", 90) : 90;
		}

		// ----- Entity settings -----
		static EntitySettings GetPlayers()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "Players", "Enabled",  true)  : true,
				s_config ? s_config->ReadFloat("Compass", "Players", "Distance", 0.0f)  : 0.0f
			};
		}

		static EntitySettings GetBaseCores()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "BaseCores", "Enabled",  true) : true,
				s_config ? s_config->ReadFloat("Compass", "BaseCores", "Distance", 0.0f) : 0.0f
			};
		}

		static MarkerSettings GetMarkers()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "Markers", "Enabled",               true)     : true,
				s_config ? s_config->ReadBool ("Compass", "Markers", "ShowAntena",             true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Markers", "AntenaDistance",         10000.0f) : 10000.0f,
				s_config ? s_config->ReadBool ("Compass", "Markers", "ShowAbandonedBase",      true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Markers", "AbandonedBaseDistance",  10000.0f) : 10000.0f,
				s_config ? s_config->ReadBool ("Compass", "Markers", "ShowCave",               true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Markers", "CaveDistance",           10000.0f) : 10000.0f,
				s_config ? s_config->ReadBool ("Compass", "Markers", "ShowObelisk",            true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Markers", "ObeliskDistance",        10000.0f) : 10000.0f,
			};
		}

		static EntitySettings GetBodies()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "Bodies", "Enabled",  true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Bodies", "Distance", 10000.0f) : 10000.0f
			};
		}

		static EntitySettings GetCustomPins()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "CustomPins", "Enabled",  true) : true,
				s_config ? s_config->ReadFloat("Compass", "CustomPins", "Distance", 0.0f) : 0.0f
			};
		}

	private:
		static IPluginConfig* s_config;
	};
}
