#pragma once

#include "plugin_interface.h"
#include <cstdio>
#include <cstring>

namespace CompassConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		// ----- General -----
		{ "General", "Enabled",                  ConfigValueType::Boolean, "false",    "Enable or disable the compass overlay" },
		{ "General", "TextOnly",                 ConfigValueType::Boolean, "false",   "Force text-only mode — never draw icon textures (useful for debugging)" },

		// ----- Compass bar -----
		{ "Compass", "Scale",                    ConfigValueType::Float,   "1.2",              "Compass size multiplier" },
		{ "Compass", "PosY",                     ConfigValueType::Float,   "60.0",             "Pixels from top of screen to the compass line" },
		{ "Compass", "WidthFraction",            ConfigValueType::Float,   "0.50",             "Half-width as fraction of screen width (0.20 = 40% total)" },
		{ "Compass", "EntityScanInterval",       ConfigValueType::Integer, "90",               "Frames between full entity scans for static things like POIs and cores (90 = ~1.5s at 60fps)" },
	{ "Compass", "PlayerScanInterval",      ConfigValueType::Integer, "3",                "Frames between player position scans (3 = ~20 updates/sec at 60fps)" },
		{ "Compass", "LineColor",                ConfigValueType::String,  "1.0, 1.0, 1.0, 0.4", "Horizontal bar colour as R, G, B, A (each 0.0-1.0). Invalid input uses the default." },

		// ----- Players -----
		{ "Players", "Enabled",                  ConfigValueType::Boolean, "true",    "Show other player markers on the compass" },
		{ "Players", "Distance",                 ConfigValueType::Float,   "0.0",     "Max render distance in UU (0 = unlimited)" },

		// ----- Base Cores -----
		{ "BaseCores", "Enabled",                ConfigValueType::Boolean, "true",    "Show base core markers on the compass" },
		{ "BaseCores", "Distance",               ConfigValueType::Float,   "20000.0",     "Max render distance in UU (0 = unlimited)" },

		// ----- Map Markers -----
		{ "Markers", "Enabled",                  ConfigValueType::Boolean, "true",    "Master toggle for all POI markers" },
		{ "Markers", "ShowAntena",               ConfigValueType::Boolean, "true",    "Show antenna POI markers" },
		{ "Markers", "AntenaDistance",           ConfigValueType::Float,   "15000.0", "Antenna max render distance in UU (0 = unlimited)" },
		{ "Markers", "ShowAbandonedBase",        ConfigValueType::Boolean, "true",    "Show abandoned base POI markers" },
		{ "Markers", "AbandonedBaseDistance",    ConfigValueType::Float,   "10000.0", "Abandoned base max render distance in UU (0 = unlimited)" },
		{ "Markers", "ShowCave",                 ConfigValueType::Boolean, "true",    "Show cave POI markers" },
		{ "Markers", "CaveDistance",             ConfigValueType::Float,   "10000.0", "Cave max render distance in UU (0 = unlimited)" },
		{ "Markers", "ShowObelisk",              ConfigValueType::Boolean, "true",    "Show obelisk POI markers" },
		{ "Markers", "ObeliskDistance",          ConfigValueType::Float,   "7500.0", "Obelisk max render distance in UU (0 = unlimited)" },

		// ----- Foundables (Dead Bodies + Drones) -----
		{ "Foundables", "Enabled",               ConfigValueType::Boolean, "true",    "Show foundable markers (dead bodies, drones) on the compass" },
		{ "Foundables", "ShowDeadBody",          ConfigValueType::Boolean, "true",    "Show dead body foundable markers" },
		{ "Foundables", "DeadBodyDistance",      ConfigValueType::Float,   "10000.0", "Dead body max render distance in UU (0 = unlimited)" },
		{ "Foundables", "ShowDrone",             ConfigValueType::Boolean, "true",    "Show drone foundable markers" },
		{ "Foundables", "DroneDistance",         ConfigValueType::Float,   "10000.0", "Drone max render distance in UU (0 = unlimited)" },

		// ----- Enemies -----
		{ "Enemies", "Enabled",                  ConfigValueType::Boolean, "true",    "Show enemy dots on the compass bar" },
		{ "Enemies", "Distance",                 ConfigValueType::Float,   "5000.0",  "Max render distance in UU (0 = unlimited)" },

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

	struct FoundableSettings
	{
		bool  enabled;
		bool  showDeadBody;
		float deadBodyDistance;
		bool  showDrone;
		float droneDistance;
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

		static int GetPlayerScanInterval()
		{
			int val = s_config ? s_config->ReadInt("Compass", "Compass", "PlayerScanInterval", 3) : 3;
			if (val < 1) val = 1;
			return val;
		}

		// Returns the horizontal bar colour from config, or the default {1,1,1,0.4} on any
		// parse error or out-of-range component. Format: "R, G, B, A" (each 0.0-1.0).
		static void GetLineColor(float& r, float& g, float& b, float& a)
		{
			// Defaults
			r = 1.0f; g = 1.0f; b = 1.0f; a = 0.4f;

			if (!s_config) return;

			char buf[64] = {};
			if (!s_config->ReadString("Compass", "Compass", "LineColor", buf, (int)sizeof(buf), ""))
				return;

			float tr, tg, tb, ta;
			// Accept "R, G, B, A" or "R,G,B,A" (with or without spaces)
			if (sscanf_s(buf, "%f , %f , %f , %f", &tr, &tg, &tb, &ta) != 4)
				return;

			// All components must be in [0, 1]
			if (tr < 0.0f || tr > 1.0f || tg < 0.0f || tg > 1.0f ||
				tb < 0.0f || tb > 1.0f || ta < 0.0f || ta > 1.0f)
				return;

			r = tr; g = tg; b = tb; a = ta;
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

		static FoundableSettings GetFoundables()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "Foundables", "Enabled",          true)     : true,
				s_config ? s_config->ReadBool ("Compass", "Foundables", "ShowDeadBody",      true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Foundables", "DeadBodyDistance",  10000.0f) : 10000.0f,
				s_config ? s_config->ReadBool ("Compass", "Foundables", "ShowDrone",         true)     : true,
				s_config ? s_config->ReadFloat("Compass", "Foundables", "DroneDistance",     10000.0f) : 10000.0f,
			};
		}

		static EntitySettings GetEnemies()
		{
			return {
				s_config ? s_config->ReadBool ("Compass", "Enemies", "Enabled",  true)    : true,
				s_config ? s_config->ReadFloat("Compass", "Enemies", "Distance", 5000.0f) : 5000.0f
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
