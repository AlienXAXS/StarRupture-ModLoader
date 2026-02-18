#include "pch.h"
#include "plugin_manager.h"
#include "plugin_interface.h"
#include "logger.h"
#include "config_manager.h"
#include "scanner_interface.h"  // Add scanner interface
#include "hooks_interface.h"    // Add hooks interface
#include <vector>
#include <string>

namespace ModLoader
{
	// Structure to hold loaded plugin information
	struct LoadedPlugin
	{
		HMODULE hModule;
		PluginInfo* info;
		GetPluginInfoFunc getInfo;
		PluginInitFunc init;
		PluginShutdownFunc shutdown;
		std::wstring fileName;
		bool isInitialized;
	};

	static std::vector<LoadedPlugin> g_loadedPlugins;
	static CRITICAL_SECTION g_pluginLock;
	static bool g_managerInitialized = false;

	// Load a single plugin DLL
	static bool LoadPlugin(const std::wstring& dllPath)
	{
		LogMessage(L"Loading plugin: %s", dllPath.c_str());

		// Load the DLL
		HMODULE hModule = LoadLibraryW(dllPath.c_str());
		if (!hModule)
		{
			DWORD error = GetLastError();
			LogMessage(L"Failed to load plugin DLL: %s (error: %lu)", dllPath.c_str(), error);
			return false;
		}

		// Get plugin functions
		GetPluginInfoFunc getInfo = reinterpret_cast<GetPluginInfoFunc>(
			GetProcAddress(hModule, PLUGIN_GET_INFO_FUNC_NAME));
		PluginInitFunc init = reinterpret_cast<PluginInitFunc>(
			GetProcAddress(hModule, PLUGIN_INIT_FUNC_NAME));
		PluginShutdownFunc shutdown = reinterpret_cast<PluginShutdownFunc>(
			GetProcAddress(hModule, PLUGIN_SHUTDOWN_FUNC_NAME));

		// Validate required functions
		if (!getInfo || !init || !shutdown)
		{
			LogMessage(L"Plugin missing required exports: %s", dllPath.c_str());
			FreeLibrary(hModule);
			return false;
		}

		// Get plugin info
		PluginInfo* info = getInfo();
		if (!info)
		{
			LogMessage(L"Plugin GetPluginInfo returned nullptr: %s", dllPath.c_str());
			FreeLibrary(hModule);
			return false;
		}

		// Validate interface version
		if (info->interfaceVersion != PLUGIN_INTERFACE_VERSION)
		{
			LogMessage(L"Plugin interface version mismatch (expected %d, got %d): %s",
				PLUGIN_INTERFACE_VERSION, info->interfaceVersion, dllPath.c_str());
			FreeLibrary(hModule);
			return false;
		}

		LogMessage(L"Plugin info - Name: %S, Version: %S, Author: %S",
			info->name, info->version, info->author);

		// Initialize the plugin with logger, config, scanner, and hooks
		if (!init(GetPluginLogger(), GetPluginConfig(), GetPluginScanner(), GetPluginHooks()))
		{
			LogMessage(L"Plugin initialization failed: %s", dllPath.c_str());
			FreeLibrary(hModule);
			return false;
		}

		// Store plugin info
		LoadedPlugin plugin = {};
		plugin.hModule = hModule;
		plugin.info = info;
		plugin.getInfo = getInfo;
		plugin.init = init;
		plugin.shutdown = shutdown;
		plugin.fileName = dllPath;
		plugin.isInitialized = true;

		EnterCriticalSection(&g_pluginLock);
		g_loadedPlugins.push_back(plugin);
		LeaveCriticalSection(&g_pluginLock);

		LogMessage(L"Successfully loaded plugin: %S v%S", info->name, info->version);
		return true;
	}

	void InitializePluginManager()
	{
		InitializeCriticalSection(&g_pluginLock);
		g_managerInitialized = true;
		LogMessage(L"Plugin manager initialized");
	}

	void ShutdownPluginManager()
	{
		g_managerInitialized = false;
		DeleteCriticalSection(&g_pluginLock);
		LogMessage(L"Plugin manager shutdown");
	}

	void LoadAllPlugins()
	{
		if (!g_managerInitialized)
		{
			LogMessage(L"ERROR: Plugin manager not initialized");
			return;
		}

		// Get the directory of the current executable
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		// Remove the filename to get the directory
		wchar_t* lastSlash = wcsrchr(exePath, L'\\');
		if (lastSlash)
		{
			*lastSlash = L'\0';
		}

		// Build path to alienx_mods directory
		wchar_t modsPath[MAX_PATH] = {};
		swprintf_s(modsPath, L"%s\\alienx_mods", exePath);

		LogMessage(L"Searching for plugins in: %s", modsPath);

		// Check if directory exists
		DWORD attribs = GetFileAttributesW(modsPath);
		if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
		{
			LogMessage(L"alienx_mods directory not found, creating it...");
			if (!CreateDirectoryW(modsPath, nullptr))
			{
				LogMessage(L"Failed to create alienx_mods directory (error: %lu)", GetLastError());
				return;
			}
		}

		// Build search pattern
		wchar_t searchPattern[MAX_PATH] = {};
		swprintf_s(searchPattern, L"%s\\*.dll", modsPath);

		// Enumerate all DLL files
		WIN32_FIND_DATAW findData = {};
		HANDLE hFind = FindFirstFileW(searchPattern, &findData);

		if (hFind == INVALID_HANDLE_VALUE)
		{
			LogMessage(L"No plugins found in alienx_mods directory");
			return;
		}

		int loadedCount = 0;
		do
		{
			// Skip directories
			if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			// Build full path to DLL
			wchar_t dllPath[MAX_PATH] = {};
			swprintf_s(dllPath, L"%s\\%s", modsPath, findData.cFileName);

			// Try to load the plugin
			if (LoadPlugin(dllPath))
			{
				loadedCount++;
			}

		} while (FindNextFileW(hFind, &findData));

		FindClose(hFind);

		LogMessage(L"Loaded %d plugin(s) from alienx_mods", loadedCount);
	}

	void UnloadAllPlugins()
	{
		LogMessage(L"Unloading all plugins...");

		EnterCriticalSection(&g_pluginLock);

		for (auto& plugin : g_loadedPlugins)
		{
			if (plugin.isInitialized)
			{
				LogMessage(L"Shutting down plugin: %S", plugin.info->name);
				plugin.shutdown();
				plugin.isInitialized = false;
			}

			if (plugin.hModule)
			{
				FreeLibrary(plugin.hModule);
				plugin.hModule = nullptr;
			}
		}

		g_loadedPlugins.clear();

		LeaveCriticalSection(&g_pluginLock);

		LogMessage(L"All plugins unloaded");
	}
}
