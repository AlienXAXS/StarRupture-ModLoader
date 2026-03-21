#include "config_manager.h"
#include "logging/logger.h"
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace ModLoaderLogger
{
	static wchar_t g_configDirectory[MAX_PATH] = {};
	static CRITICAL_SECTION g_configLock;
	static bool g_configInitialized = false;

	// ---------------------------------------------------------------------------
	// File-mtime cache — eliminates per-frame GetPrivateProfileStringW calls.
	// The mtime of each INI file is checked at most every 500 ms. On change,
	// the cache for that file is cleared so values repopulate on the next read,
	// giving transparent live-reload to all plugins at no per-frame cost.
	// Must always be accessed with g_configLock held.
	// ---------------------------------------------------------------------------

	struct PluginFileCache
	{
		FILETIME  lastMtime        = {};
		ULONGLONG lastMtimeCheckMs = 0; // GetTickCount64() of last mtime check
		std::unordered_map<std::string, std::string> entries; // "Section\nKey" → value
	};

	static std::unordered_map<std::wstring, PluginFileCache> g_fileCache;
	static constexpr ULONGLONG kMtimeCheckIntervalMs = 500;

	static std::unordered_map<std::string, const ConfigSchema*> g_schemas;

	static FILETIME GetFileMtime(const wchar_t* path)
	{
		WIN32_FILE_ATTRIBUTE_DATA info = {};
		GetFileAttributesExW(path, GetFileExInfoStandard, &info);
		return info.ftLastWriteTime;
	}

	// Returns cached string for (path, "Section\nKey"), or nullptr on miss.
	// Invalidates the entire file cache if the file's mtime has changed.
	// Must be called with g_configLock held.
	static const std::string* CacheLookup(PluginFileCache& fc, const wchar_t* path, const std::string& cacheKey)
	{
		const ULONGLONG now = GetTickCount64();
		if (now - fc.lastMtimeCheckMs >= kMtimeCheckIntervalMs)
		{
			fc.lastMtimeCheckMs = now;
			const FILETIME mt = GetFileMtime(path);
			if (mt.dwLowDateTime != fc.lastMtime.dwLowDateTime ||
				mt.dwHighDateTime != fc.lastMtime.dwHighDateTime)
			{
				fc.lastMtime = mt;
				fc.entries.clear(); // file changed on disk — repopulate on demand
			}
		}

		auto it = fc.entries.find(cacheKey);
		return (it != fc.entries.end()) ? &it->second : nullptr;
	}

	// Build path to plugin's config file
	static bool GetPluginConfigPath(const char* pluginName, wchar_t* outPath, int maxLen)
	{
		if (!pluginName || !outPath || maxLen < MAX_PATH)
			return false;

		// Convert plugin name to wide string
		wchar_t wPluginName[256];
		MultiByteToWideChar(CP_UTF8, 0, pluginName, -1, wPluginName, 256);

		// Build path: Plugins\config\PluginName.ini
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

		// Build "Section\nKey" cache key
		std::string cacheKey;
		cacheKey.reserve(strlen(section) + 1 + strlen(key));
		cacheKey += section;
		cacheKey += '\n';
		cacheKey += key;

		EnterCriticalSection(&g_configLock);

		PluginFileCache& fc = g_fileCache[configPath];
		if (const std::string* cached = CacheLookup(fc, configPath, cacheKey))
		{
			strncpy_s(outValue, maxLen, cached->c_str(), _TRUNCATE);
			LeaveCriticalSection(&g_configLock);
			return true;
		}

		// Cache miss — read from INI, populate cache
		wchar_t wSection[256], wKey[256];
		MultiByteToWideChar(CP_UTF8, 0, section, -1, wSection, 256);
		MultiByteToWideChar(CP_UTF8, 0, key, -1, wKey, 256);

		wchar_t wDefault[1024] = L"";
		if (defaultValue)
			MultiByteToWideChar(CP_UTF8, 0, defaultValue, -1, wDefault, 1024);

		wchar_t wValue[1024];
		GetPrivateProfileStringW(wSection, wKey, wDefault, wValue, 1024, configPath);

		WideCharToMultiByte(CP_UTF8, 0, wValue, -1, outValue, maxLen, nullptr, nullptr);

		fc.entries[cacheKey] = outValue;

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

		wchar_t wSection[256], wKey[256], wValue[1024];
		MultiByteToWideChar(CP_UTF8, 0, section, -1, wSection, 256);
		MultiByteToWideChar(CP_UTF8, 0, key, -1, wKey, 256);
		MultiByteToWideChar(CP_UTF8, 0, value, -1, wValue, 1024);

		EnterCriticalSection(&g_configLock);
		BOOL result = WritePrivateProfileStringW(wSection, wKey, wValue, configPath);

		// Invalidate the cached entry so the next read reflects the write immediately,
		// without waiting for the 500 ms mtime check to fire.
		if (result)
		{
			std::string cacheKey;
			cacheKey.reserve(strlen(section) + 1 + strlen(key));
			cacheKey += section;
			cacheKey += '\n';
			cacheKey += key;
			auto it = g_fileCache.find(configPath);
			if (it != g_fileCache.end())
				it->second.entries.erase(cacheKey);
		}

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
		char buf[16] = {};
		const char* def = defaultValue ? "1" : "0";
		if (!ConfigReadString(pluginName, section, key, buf, sizeof(buf), def))
			return defaultValue;
		return (_stricmp(buf, "true") == 0 ||
		        _stricmp(buf, "yes")  == 0 ||
		        strcmp(buf, "1")      == 0);
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

	// Write a config file from the schema, with "; description" comment lines above each key.
	// Uses _wfopen directly so the file is clean UTF-8/ASCII from the start,
	// avoiding any encoding quirks from WritePrivateProfileStringW.
	//
	// overrides: optional map of "Section\nKey" -> raw value string.
	//   When a key is present in the map its existing value is written instead of
	//   the schema default, preserving user settings when reformatting an existing file.
	static void WriteFormattedConfig(const ConfigSchema* schema, const wchar_t* configPath,
		const std::unordered_map<std::string, std::string>* overrides = nullptr)
	{
		std::string content;
		const char* lastSection = nullptr;

		for (int i = 0; i < schema->entryCount; ++i)
		{
			const ConfigEntry& entry = schema->entries[i];

			if (!lastSection || strcmp(lastSection, entry.section) != 0)
			{
				if (lastSection)
					content += "\r\n"; // blank line between sections
				content += "[";
				content += entry.section;
				content += "]\r\n";
				lastSection = entry.section;
			}

			// Determine the value to write: prefer override, fall back to schema default
			std::string valueStr;
			if (overrides)
			{
				std::string cacheKey = entry.section;
				cacheKey += '\n';
				cacheKey += entry.key;
				auto it = overrides->find(cacheKey);
				if (it != overrides->end())
					valueStr = it->second;
			}

			if (valueStr.empty())
			{
				char valueBuf[64] = {};
				switch (entry.type)
				{
				case ConfigValueType::Boolean:
				{
					bool b = (_stricmp(entry.defaultValue, "true") == 0 ||
						_stricmp(entry.defaultValue, "1") == 0 ||
						_stricmp(entry.defaultValue, "yes") == 0);
					snprintf(valueBuf, sizeof(valueBuf), "%d", b ? 1 : 0);
					break;
				}
				case ConfigValueType::Integer:
					snprintf(valueBuf, sizeof(valueBuf), "%d", atoi(entry.defaultValue));
					break;
				case ConfigValueType::Float:
					snprintf(valueBuf, sizeof(valueBuf), "%.6f", static_cast<float>(atof(entry.defaultValue)));
					break;
				default:
					snprintf(valueBuf, sizeof(valueBuf), "%s", entry.defaultValue);
					break;
				}
				valueStr = valueBuf;
			}

			if (entry.description && entry.description[0])
			{
				content += "; ";
				content += entry.description;
				content += "\r\n";
			}

			content += entry.key;
			content += "=";
			content += valueStr;
			content += "\r\n";
		}

		EnterCriticalSection(&g_configLock);

		// Skip the write if the file already matches the generated content
		bool needsWrite = true;
		FILE* existing = nullptr;
		if (_wfopen_s(&existing, configPath, L"rb") == 0 && existing)
		{
			fseek(existing, 0, SEEK_END);
			long fileSize = ftell(existing);
			fseek(existing, 0, SEEK_SET);
			if (fileSize == static_cast<long>(content.size()))
			{
				std::string existingContent(fileSize, '\0');
				fread(&existingContent[0], 1, fileSize, existing);
				needsWrite = (existingContent != content);
			}
			fclose(existing);
		}

		if (needsWrite)
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, configPath, L"wb") == 0 && f)
			{
				fwrite(content.c_str(), 1, content.size(), f);
				fclose(f);
			}
		}

		LeaveCriticalSection(&g_configLock);
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

				LogDebug(L"[ConfigManager] Added missing config entry: %S.%S = %S",
					entry.section, entry.key, entry.defaultValue);
			}
		}

		if (addedCount > 0)
		{
			LogDebug(L"[ConfigManager] Validated config for '%s': added %d missing entries",
				wPluginName, addedCount);
		}
		else
		{
			LogDebug(L"[ConfigManager] Config for '%s' is complete", wPluginName);
		}
	}

	// Initialize config from schema
	static bool ConfigInitializeFromSchema(const char* pluginName, const ConfigSchema* schema)
	{
		if (!g_configInitialized || !pluginName || !schema || !schema->entries)
		{
			LogError(L"[ConfigManager] InitializeFromSchema failed: invalid parameters");
			return false;
		}

		g_schemas[pluginName] = schema;

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
			LogDebug(L"[ConfigManager] Creating new config for '%s' with %d entries", wPluginName, schema->entryCount);
			WriteFormattedConfig(schema, configPath);
			LogDebug(L"[ConfigManager] Config created: %s", configPath);
			return true;
		}
		else
		{
			// Read all current values from the existing file, then rewrite it with
			// comment lines above each key. This adds comments to existing configs
			// without losing any user-configured values.
			LogDebug(L"[ConfigManager] Reformatting config for '%s' with comments, preserving values...", wPluginName);

			std::unordered_map<std::string, std::string> currentValues;
			wchar_t wSection[256], wKey[256], wValue[1024];
			const wchar_t* sentinel = L"__MISSING__";

			for (int i = 0; i < schema->entryCount; ++i)
			{
				const ConfigEntry& entry = schema->entries[i];
				MultiByteToWideChar(CP_UTF8, 0, entry.section, -1, wSection, 256);
				MultiByteToWideChar(CP_UTF8, 0, entry.key,     -1, wKey,     256);

				EnterCriticalSection(&g_configLock);
				GetPrivateProfileStringW(wSection, wKey, sentinel, wValue, 1024, configPath);
				LeaveCriticalSection(&g_configLock);

				if (wcscmp(wValue, sentinel) != 0)
				{
					char value[1024];
					WideCharToMultiByte(CP_UTF8, 0, wValue, -1, value, sizeof(value), nullptr, nullptr);
					std::string cacheKey = entry.section;
					cacheKey += '\n';
					cacheKey += entry.key;
					currentValues[cacheKey] = value;
				}
			}

			FILETIME mtimeBefore = GetFileMtime(configPath);
			WriteFormattedConfig(schema, configPath, &currentValues);
			FILETIME mtimeAfter = GetFileMtime(configPath);

			bool fileChanged = (mtimeBefore.dwLowDateTime  != mtimeAfter.dwLowDateTime ||
			                    mtimeBefore.dwHighDateTime != mtimeAfter.dwHighDateTime);

			if (fileChanged)
			{
				// Invalidate the file cache so the next read picks up the rewritten file
				EnterCriticalSection(&g_configLock);
				g_fileCache.erase(configPath);
				LeaveCriticalSection(&g_configLock);
				LogDebug(L"[ConfigManager] Config reformatted for '%s' (%zu values preserved)",
					wPluginName, currentValues.size());
			}
			else
			{
				LogDebug(L"[ConfigManager] Config for '%s' already up to date, no rewrite needed", wPluginName);
			}
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
		swprintf_s(g_configDirectory, L"%s\\Plugins\\config", exePath);

		// Create directory if it doesn't exist
		DWORD attribs = GetFileAttributesW(g_configDirectory);
		if (attribs == INVALID_FILE_ATTRIBUTES)
		{
			CreateDirectoryW(g_configDirectory, nullptr);
			LogDebug(L"Created config directory: %s", g_configDirectory);
		}

		g_configInitialized = true;
		LogInfo(L"Config manager initialized: %s", g_configDirectory);
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

	const ConfigSchema* GetPluginSchema(const char* pluginName)
	{
		if (!pluginName) return nullptr;
		auto it = g_schemas.find(pluginName);
		return (it != g_schemas.end()) ? it->second : nullptr;
	}
}
