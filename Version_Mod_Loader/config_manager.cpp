#include "pch.h"
#include "config_manager.h"
#include "logger.h"
#include <string>
#include <fstream>
#include <sstream>

namespace ModLoader
{
	static wchar_t g_configDirectory[MAX_PATH] = {};
	static CRITICAL_SECTION g_configLock;
	static bool g_configInitialized = false;

	// Build path to plugin's config file
	static bool GetPluginConfigPath(const char* pluginName, wchar_t* outPath, int maxLen)
	{
		if (!pluginName || !outPath || maxLen < MAX_PATH)
			return false;

		// Convert plugin name to wide string
		wchar_t wPluginName[256];
		MultiByteToWideChar(CP_UTF8, 0, pluginName, -1, wPluginName, 256);

		// Build path: alienx_mods\config\PluginName.ini
		swprintf_s(outPath, maxLen, L"%s\\%s.ini", g_configDirectory, wPluginName);
		return true;
	}

	// Implementation functions
	static bool ConfigReadString(const char* pluginName, const char* section, const char* key, char* outValue, int maxLen, const char* defaultValue)
	{
		if (!g_configInitialized || !pluginName || !section || !key || !outValue)
			return false;

		wchar_t configPath[MAX_PATH];
		if (!GetPluginConfigPath(pluginName, configPath, MAX_PATH))
			return false;

		// Convert section and key to wide strings
		wchar_t wSection[256], wKey[256];
		MultiByteToWideChar(CP_UTF8, 0, section, -1, wSection, 256);
		MultiByteToWideChar(CP_UTF8, 0, key, -1, wKey, 256);

		// Convert default value to wide string
		wchar_t wDefault[1024] = L"";
		if (defaultValue)
		{
			MultiByteToWideChar(CP_UTF8, 0, defaultValue, -1, wDefault, 1024);
		}

		EnterCriticalSection(&g_configLock);

		wchar_t wValue[1024];
		GetPrivateProfileStringW(wSection, wKey, wDefault, wValue, 1024, configPath);

		// Convert back to UTF8
		WideCharToMultiByte(CP_UTF8, 0, wValue, -1, outValue, maxLen, nullptr, nullptr);

		LeaveCriticalSection(&g_configLock);
		return true;
	}

	static bool ConfigWriteString(const char* pluginName, const char* section, const char* key, const char* value)
	{
		if (!g_configInitialized || !pluginName || !section || !key || !value)
			return false;

		wchar_t configPath[MAX_PATH];
		if (!GetPluginConfigPath(pluginName, configPath, MAX_PATH))
			return false;

		// Convert to wide strings
		wchar_t wSection[256], wKey[256], wValue[1024];
		MultiByteToWideChar(CP_UTF8, 0, section, -1, wSection, 256);
		MultiByteToWideChar(CP_UTF8, 0, key, -1, wKey, 256);
		MultiByteToWideChar(CP_UTF8, 0, value, -1, wValue, 1024);

		EnterCriticalSection(&g_configLock);
		BOOL result = WritePrivateProfileStringW(wSection, wKey, wValue, configPath);
		LeaveCriticalSection(&g_configLock);

		return result != 0;
	}

	static int ConfigReadInt(const char* pluginName, const char* section, const char* key, int defaultValue)
	{
		if (!g_configInitialized || !pluginName || !section || !key)
			return defaultValue;

		wchar_t configPath[MAX_PATH];
		if (!GetPluginConfigPath(pluginName, configPath, MAX_PATH))
			return defaultValue;

		// Convert to wide strings
		wchar_t wSection[256], wKey[256];
		MultiByteToWideChar(CP_UTF8, 0, section, -1, wSection, 256);
		MultiByteToWideChar(CP_UTF8, 0, key, -1, wKey, 256);

		EnterCriticalSection(&g_configLock);
		int value = GetPrivateProfileIntW(wSection, wKey, defaultValue, configPath);
		LeaveCriticalSection(&g_configLock);

		return value;
	}

	static bool ConfigWriteInt(const char* pluginName, const char* section, const char* key, int value)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%d", value);
		return ConfigWriteString(pluginName, section, key, buffer);
	}

	static float ConfigReadFloat(const char* pluginName, const char* section, const char* key, float defaultValue)
	{
		char defaultStr[32];
		snprintf(defaultStr, sizeof(defaultStr), "%.6f", defaultValue);

		char valueStr[32];
		if (!ConfigReadString(pluginName, section, key, valueStr, sizeof(valueStr), defaultStr))
			return defaultValue;

		return static_cast<float>(atof(valueStr));
	}

	static bool ConfigWriteFloat(const char* pluginName, const char* section, const char* key, float value)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%.6f", value);
		return ConfigWriteString(pluginName, section, key, buffer);
	}

	static bool ConfigReadBool(const char* pluginName, const char* section, const char* key, bool defaultValue)
	{
		return ConfigReadInt(pluginName, section, key, defaultValue ? 1 : 0) != 0;
	}

	static bool ConfigWriteBool(const char* pluginName, const char* section, const char* key, bool value)
	{
		return ConfigWriteInt(pluginName, section, key, value ? 1 : 0);
	}

	// Check if a key exists in the config file
	static bool ConfigKeyExists(const char* pluginName, const char* section, const char* key)
	{
		if (!g_configInitialized || !pluginName || !section || !key)
			return false;

		wchar_t configPath[MAX_PATH];
		if (!GetPluginConfigPath(pluginName, configPath, MAX_PATH))
			return false;

		// Convert section and key to wide strings
		wchar_t wSection[256], wKey[256];
		MultiByteToWideChar(CP_UTF8, 0, section, -1, wSection, 256);
		MultiByteToWideChar(CP_UTF8, 0, key, -1, wKey, 256);

		// Use a unique default that's unlikely to be a real value
		const wchar_t* uniqueDefault = L"__CONFIG_KEY_NOT_FOUND__";
		wchar_t wValue[1024];
		
		EnterCriticalSection(&g_configLock);
		GetPrivateProfileStringW(wSection, wKey, uniqueDefault, wValue, 1024, configPath);
		LeaveCriticalSection(&g_configLock);

		return wcscmp(wValue, uniqueDefault) != 0;
	}

	// Helper to convert default value string to appropriate type and write
	static void WriteDefaultValue(const char* pluginName, const ConfigEntry& entry)
	{
		switch (entry.type)
		{
		case ConfigValueType::String:
			ConfigWriteString(pluginName, entry.section, entry.key, entry.defaultValue);
			break;
		case ConfigValueType::Integer:
			ConfigWriteInt(pluginName, entry.section, entry.key, atoi(entry.defaultValue));
			break;
		case ConfigValueType::Float:
			ConfigWriteFloat(pluginName, entry.section, entry.key, static_cast<float>(atof(entry.defaultValue)));
			break;
		case ConfigValueType::Boolean:
			{
				bool boolVal = (_stricmp(entry.defaultValue, "true") == 0 || 
							   _stricmp(entry.defaultValue, "1") == 0 ||
							   _stricmp(entry.defaultValue, "yes") == 0);
				ConfigWriteBool(pluginName, entry.section, entry.key, boolVal);
			}
			break;
		}
	}

	// Validate config and add missing entries
	static void ConfigValidateConfig(const char* pluginName, const ConfigSchema* schema)
	{
		if (!g_configInitialized || !pluginName || !schema || !schema->entries)
			return;

		wchar_t wPluginName[256];
		MultiByteToWideChar(CP_UTF8, 0, pluginName, -1, wPluginName, 256);

		int addedCount = 0;

		for (int i = 0; i < schema->entryCount; ++i)
		{
			const ConfigEntry& entry = schema->entries[i];

			// Check if key exists
			if (!ConfigKeyExists(pluginName, entry.section, entry.key))
			{
				// Key missing - add with default value
				WriteDefaultValue(pluginName, entry);
				addedCount++;

				LogMessage(L"[ConfigManager] Added missing config entry: %S.%S = %S", 
					entry.section, entry.key, entry.defaultValue);
			}
		}

		if (addedCount > 0)
		{
			LogMessage(L"[ConfigManager] Validated config for '%s': added %d missing entries", 
				wPluginName, addedCount);
		}
		else
		{
			LogMessage(L"[ConfigManager] Config for '%s' is complete", wPluginName);
		}
	}

	// Initialize config from schema
	static bool ConfigInitializeFromSchema(const char* pluginName, const ConfigSchema* schema)
	{
		if (!g_configInitialized || !pluginName || !schema || !schema->entries)
		{
			LogMessage(L"[ConfigManager] InitializeFromSchema failed: invalid parameters");
			return false;
		}

		wchar_t configPath[MAX_PATH];
		if (!GetPluginConfigPath(pluginName, configPath, MAX_PATH))
			return false;

		// Convert plugin name for logging
		wchar_t wPluginName[256];
		MultiByteToWideChar(CP_UTF8, 0, pluginName, -1, wPluginName, 256);

		// Check if config file exists
		bool configExists = (GetFileAttributesW(configPath) != INVALID_FILE_ATTRIBUTES);

		if (!configExists)
		{
			LogMessage(L"[ConfigManager] Creating new config for '%s' with %d entries", wPluginName, schema->entryCount);

			// Create config file with all defaults
			for (int i = 0; i < schema->entryCount; ++i)
			{
				const ConfigEntry& entry = schema->entries[i];

				// Write description as comment if provided
				if (entry.description && entry.description[0] != '\0')
				{
					// Write comment line (INI files support ; for comments)
					wchar_t wSection[256];
					MultiByteToWideChar(CP_UTF8, 0, entry.section, -1, wSection, 256);
					
					// Note: WritePrivateProfileString doesn't support comments,
					// but we can document this in a readme or use a separate comment system
				}

				// Write default value
				WriteDefaultValue(pluginName, entry);
			}

			LogMessage(L"[ConfigManager] Config created: %s", configPath);
			return true;
		}
		else
		{
			LogMessage(L"[ConfigManager] Config exists for '%s', validating entries...", wPluginName);
			ConfigValidateConfig(pluginName, schema);
			return true;
		}
	}

	// Global config interface instance
	static IPluginConfig g_pluginConfig = {
		ConfigReadString,
		ConfigWriteString,
		ConfigReadInt,
		ConfigWriteInt,
		ConfigReadFloat,
		ConfigWriteFloat,
		ConfigReadBool,
		ConfigWriteBool,
		ConfigInitializeFromSchema,
		ConfigValidateConfig
	};

	void InitializeConfigManager()
	{
		InitializeCriticalSection(&g_configLock);

		// Get the directory of the current executable
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		// Remove the filename to get the directory
		wchar_t* lastSlash = wcsrchr(exePath, L'\\');
		if (lastSlash)
		{
			*lastSlash = L'\0';
		}

		// Build path to config directory
		swprintf_s(g_configDirectory, L"%s\\alienx_mods\\config", exePath);

		// Create directory if it doesn't exist
		DWORD attribs = GetFileAttributesW(g_configDirectory);
		if (attribs == INVALID_FILE_ATTRIBUTES)
		{
			CreateDirectoryW(g_configDirectory, nullptr);
			LogMessage(L"Created config directory: %s", g_configDirectory);
		}

		g_configInitialized = true;
		LogMessage(L"Config manager initialized: %s", g_configDirectory);
	}

	void ShutdownConfigManager()
	{
		g_configInitialized = false;
		DeleteCriticalSection(&g_configLock);
	}

	const wchar_t* GetConfigDirectory()
	{
		return g_configDirectory;
	}

	IPluginConfig* GetPluginConfig()
	{
		return &g_pluginConfig;
	}
}
