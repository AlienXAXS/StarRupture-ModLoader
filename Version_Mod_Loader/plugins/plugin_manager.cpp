#include "plugin_manager.h"

#include <memory>

#include "plugin_interface.h"
#include "logging/log.h"
#include "logging/logger.h"
#include "logging/logger_interface.h"
#include "config/config_manager.h"
#include "memory_scanner/scanner_interface.h"
#include "hooks/hooks_interface.h"
#include <vector>
#include <string>

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/splash_window.h"
#endif

namespace PluginManager
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

		// Cached display strings — valid even after FreeLibrary.
		std::string cachedName;
		std::string cachedVersion;
		std::string cachedAuthor;

		bool isOutOfDate;  // true when DLL was found but rejected due to version mismatch

		// Stable identity struct passed to PluginInit and retained by the plugin->
		// name/version point into cachedName/cachedVersion so they outlive PluginInfo.
		IPluginSelf self;
	};

	std::vector<std::unique_ptr<LoadedPlugin>> g_loadedPlugins;
	static CRITICAL_SECTION g_pluginLock;
	static bool g_managerInitialized = false;
	static bool   g_startupComplete    = false;
	static HANDLE g_initCompleteEvent  = NULL;

	// Update self pointers after loading or reloading a plugin, so they point to the cached strings.
	static void RefreshSelfPointers(LoadedPlugin& rec)
	{
		rec.self.name = rec.cachedName.c_str();
		rec.self.version = rec.cachedVersion.c_str();
	}

	// Inner load helper: performs LoadLibrary, GetProcAddress, GetPluginInfo,
	// version check, and PluginInit on an existing LoadedPlugin record.
	// rec.fileName must be set before calling. On success: hModule, info,
	// function pointers, cached strings, and isInitialized are all populated.
	// On failure: any resources acquired are released and the record is left clean.
	static bool LoadPluginIntoRecord(LoadedPlugin& rec)
	{
		ModLoaderLogger::LogMessage(L"Loading plugin: %s", rec.fileName.c_str());

		HMODULE hModule = LoadLibraryW(rec.fileName.c_str());
		if (!hModule)
		{
			DWORD error = GetLastError();
			ModLoaderLogger::LogMessage(L"Failed to load plugin DLL: %s (error: %lu)", rec.fileName.c_str(), error);
			return false;
		}

		GetPluginInfoFunc getInfo = reinterpret_cast<GetPluginInfoFunc>(
			GetProcAddress(hModule, PLUGIN_GET_INFO_FUNC_NAME));
		PluginInitFunc init = reinterpret_cast<PluginInitFunc>(
			GetProcAddress(hModule, PLUGIN_INIT_FUNC_NAME));
		PluginShutdownFunc shutdown = reinterpret_cast<PluginShutdownFunc>(
			GetProcAddress(hModule, PLUGIN_SHUTDOWN_FUNC_NAME));

		if (!getInfo || !init || !shutdown)
		{
			ModLoaderLogger::LogMessage(L"Plugin missing required exports: %s", rec.fileName.c_str());
			FreeLibrary(hModule);
			return false;
		}

		PluginInfo* info = getInfo();
		if (!info)
		{
			ModLoaderLogger::LogMessage(L"Plugin GetPluginInfo returned nullptr: %s", rec.fileName.c_str());
			FreeLibrary(hModule);
			return false;
		}

		if (info->interfaceVersion < PLUGIN_INTERFACE_VERSION_MIN ||
			info->interfaceVersion > PLUGIN_INTERFACE_VERSION_MAX)
		{
			ModLoaderLogger::LogMessage(L"Plugin interface version %d not in supported range [%d, %d]: %s",
				info->interfaceVersion,
				PLUGIN_INTERFACE_VERSION_MIN, PLUGIN_INTERFACE_VERSION_MAX,
				rec.fileName.c_str());
			// Cache what we can from the DLL so the UI can display it.
			rec.cachedName    = info->name    ? info->name    : "";
			rec.cachedVersion = info->version ? info->version : "";
			rec.cachedAuthor  = info->author  ? info->author  : "";
			rec.isOutOfDate   = true;
			FreeLibrary(hModule);
			return false;
		}

		ModLoaderLogger::LogMessage(L"Plugin info - Name: %S, Version: %S, Author: %S",
			info->name, info->version, info->author);

		rec.hModule        = hModule;
		rec.info           = info;
		rec.getInfo        = getInfo;
		rec.init           = init;
		rec.shutdown       = shutdown;
		rec.isInitialized  = false;  // deferred -- InitAllLoadedPlugins() calls PluginInit
		rec.cachedName     = info->name    ? info->name    : "";
		rec.cachedVersion  = info->version ? info->version : "";
		rec.cachedAuthor   = info->author  ? info->author  : "";

		ModLoaderLogger::LogMessage(L"Successfully loaded plugin DLL: %S v%S (PluginInit deferred)", info->name, info->version);
		return true;
	}

	// Load a single plugin DLL by path (used during startup scan).
	static bool LoadPlugin(const std::wstring& dllPath)
	{
		auto rec = std::make_unique<LoadedPlugin>();
		rec->fileName = dllPath;
		rec->isOutOfDate = false;

		if (!LoadPluginIntoRecord(*rec))
		{
			// If version check populated cached strings, keep the record visible in the UI.
			if (rec->isOutOfDate)
			{
				EnterCriticalSection(&g_pluginLock);
				g_loadedPlugins.push_back(std::move(rec));
				LeaveCriticalSection(&g_pluginLock);
			}
			return false;
		}

		RefreshSelfPointers(*rec);
		rec->self.name = rec->cachedName.c_str();
		rec->self.version = rec->cachedVersion.c_str();
		rec->self.logger = nullptr;
		rec->self.config = nullptr;
		rec->self.scanner = nullptr;
		rec->self.hooks = nullptr;

		EnterCriticalSection(&g_pluginLock);
		g_loadedPlugins.push_back(std::move(rec));
		LeaveCriticalSection(&g_pluginLock);
		return true;
	}

	void InitializePluginManager()
	{
		InitializeCriticalSection(&g_pluginLock);
		g_initCompleteEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_managerInitialized = true;
		ModLoaderLogger::LogMessage(L"Plugin manager initialized");
	}

	void ShutdownPluginManager()
	{
		g_managerInitialized = false;
		DeleteCriticalSection(&g_pluginLock);
		if (g_initCompleteEvent) { CloseHandle(g_initCompleteEvent); g_initCompleteEvent = NULL; }
		ModLoaderLogger::LogMessage(L"Plugin manager shutdown");
	}

	void LoadAllPlugins()
	{
		if (!g_managerInitialized)
		{
			ModLoaderLogger::LogMessage(L"ERROR: Plugin manager not initialized");
			return;
		}

		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		wchar_t* lastSlash = wcsrchr(exePath, L'\\');
		if (lastSlash) *lastSlash = L'\0';

		wchar_t modsPath[MAX_PATH] = {};
		swprintf_s(modsPath, L"%s\\Plugins", exePath);

		ModLoaderLogger::LogMessage(L"Searching for plugins in: %s", modsPath);

		DWORD attribs = GetFileAttributesW(modsPath);
		if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
		{
			ModLoaderLogger::LogMessage(L"Plugins directory not found, creating it...");
			if (!CreateDirectoryW(modsPath, nullptr))
			{
				ModLoaderLogger::LogMessage(L"Failed to create Plugins directory (error: %lu)", GetLastError());
				return;
			}
		}

		// Pre-collect paths so we know the total count for progress reporting.
		wchar_t searchPattern[MAX_PATH] = {};
		swprintf_s(searchPattern, L"%s\\*.dll", modsPath);

		std::vector<std::wstring> dllPaths;
		WIN32_FIND_DATAW findData = {};
		HANDLE hFind = FindFirstFileW(searchPattern, &findData);
		if (hFind != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					wchar_t dllPath[MAX_PATH] = {};
					swprintf_s(dllPath, L"%s\\%s", modsPath, findData.cFileName);
					dllPaths.push_back(dllPath);
				}
			} while (FindNextFileW(hFind, &findData));
			FindClose(hFind);
		}

		if (dllPaths.empty())
		{
			ModLoaderLogger::LogMessage(L"No plugins found in Plugins directory");
			return;
		}

		int total       = static_cast<int>(dllPaths.size());
		int loadedCount = 0;

		for (int i = 0; i < total; ++i)
		{
			// Extract bare filename for the status label.
			const wchar_t* fileName = dllPaths[i].c_str();
			const wchar_t* slash    = wcsrchr(fileName, L'\\');
			const wchar_t* name     = slash ? slash + 1 : fileName;

#ifdef MODLOADER_CLIENT_BUILD
			wchar_t msg[128];
			swprintf_s(msg, L"Loading %s (%d/%d)", name, i + 1, total);
			Splash::SetSubStatus(msg);
			Splash::SetSubProgress(static_cast<float>(i) / static_cast<float>(total));
#endif

			if (LoadPlugin(dllPaths[i]))
				loadedCount++;
		}

#ifdef MODLOADER_CLIENT_BUILD
		Splash::ClearSubBar();
#endif

		ModLoaderLogger::LogMessage(L"Loaded %d plugin DLL(s) from Plugins (PluginInit deferred)", loadedCount);
	}

	void InitAllLoadedPlugins()
	{
		if (!g_managerInitialized)
		{
			ModLoaderLogger::LogMessage(L"ERROR: Plugin manager not initialized");
			return;
		}

		EnterCriticalSection(&g_pluginLock);

		// Pre-count deferred plugins so we can show accurate (i/total) progress.
		int totalDeferred = 0;
		for (const auto& plugin : g_loadedPlugins)
			if (!plugin->isInitialized && plugin->init) totalDeferred++;

		int initCount = 0;
		int failCount = 0;
		int current   = 0;

		for (auto& plugin : g_loadedPlugins)
		{
			if (plugin->isInitialized || !plugin->init)
				continue;

			current++;
			ModLoaderLogger::LogMessage(L"Calling PluginInit for: %S v%S", plugin->cachedName.c_str(), plugin->cachedVersion.c_str());

#ifdef MODLOADER_CLIENT_BUILD
			{
				wchar_t msg[128];
				swprintf_s(msg, L"Initializing %S (%d/%d)", plugin->cachedName.c_str(), current, totalDeferred);
				Splash::SetSubStatus(msg);
				Splash::SetSubProgress(static_cast<float>(current - 1) / static_cast<float>(totalDeferred));
			}
#endif

			plugin->self.logger  = ModLoaderLogger::GetPluginLogger();
			plugin->self.config  = ModLoaderLogger::GetPluginConfig();
			plugin->self.scanner = ModLoaderLogger::GetPluginScanner();
			plugin->self.hooks   = ModLoaderLogger::GetPluginHooks();

			if (plugin->init(&plugin->self))
			{
				plugin->isInitialized = true;
				initCount++;
				ModLoaderLogger::LogMessage(L"Plugin initialized: %S", plugin->cachedName.c_str());
			}
			else
			{
				failCount++;
				ModLoaderLogger::LogMessage(L"Plugin initialization failed: %S", plugin->cachedName.c_str());
			}
		}

		LeaveCriticalSection(&g_pluginLock);

#ifdef MODLOADER_CLIENT_BUILD
		Splash::ClearSubBar();
#endif

		ModLoaderLogger::LogMessage(L"InitAllLoadedPlugins: %d initialized, %d failed (of %d deferred)", initCount, failCount, totalDeferred);
		if (g_initCompleteEvent) SetEvent(g_initCompleteEvent);
	}

	void UnloadAllPlugins()
	{
		ModLoaderLogger::LogMessage(L"Unloading all plugins...");

		EnterCriticalSection(&g_pluginLock);

		for (auto& plugin : g_loadedPlugins)
		{
			if (plugin->isInitialized)
			{
				ModLoaderLogger::LogMessage(L"Shutting down plugin: %S", plugin->cachedName.c_str());
				plugin->shutdown();
				plugin->isInitialized = false;
			}

			if (plugin->hModule)
			{
				FreeLibrary(plugin->hModule);
				plugin->hModule = nullptr;
				plugin->info    = nullptr;
			}
		}

		g_loadedPlugins.clear();

		LeaveCriticalSection(&g_pluginLock);

		ModLoaderLogger::LogMessage(L"All plugins unloaded");
	}

	int GetLoadedPluginCount()
	{
		EnterCriticalSection(&g_pluginLock);
		int count = 0;
		for (const auto& p : g_loadedPlugins)
			if (p->isInitialized) count++;
		LeaveCriticalSection(&g_pluginLock);
		return count;
	}

	int GetLoadedPluginInfos(const PluginInfo** outInfos, int maxCount)
	{
		EnterCriticalSection(&g_pluginLock);
		int total = 0;
		for (int i = 0; i < static_cast<int>(g_loadedPlugins.size()); ++i)
		{
			if (!g_loadedPlugins[i]->isInitialized) continue;
			if (outInfos && total < maxCount)
				outInfos[total] = g_loadedPlugins[i]->info;
			total++;
		}
		LeaveCriticalSection(&g_pluginLock);
		return total;
	}

	int GetAllPluginStatuses(PluginStatus* out, int maxCount)
	{
		EnterCriticalSection(&g_pluginLock);
		int total = static_cast<int>(g_loadedPlugins.size());
		if (out && maxCount > 0)
		{
			int toCopy = total < maxCount ? total : maxCount;
			for (int i = 0; i < toCopy; ++i)
			{
				const LoadedPlugin& p = *g_loadedPlugins[i];
				strncpy_s(out[i].name,    p.cachedName.c_str(),    _TRUNCATE);
				strncpy_s(out[i].version, p.cachedVersion.c_str(), _TRUNCATE);
				strncpy_s(out[i].author,  p.cachedAuthor.c_str(),  _TRUNCATE);
				out[i].isLoaded     = p.isInitialized;
				out[i].isOutOfDate  = p.isOutOfDate;
			}
		}
		LeaveCriticalSection(&g_pluginLock);
		return total;
	}

	bool UnloadPlugin(int index)
	{
		EnterCriticalSection(&g_pluginLock);

		if (index < 0 || index >= static_cast<int>(g_loadedPlugins.size()) ||
			!g_loadedPlugins[index]->isInitialized)
		{
			LeaveCriticalSection(&g_pluginLock);
			return false;
		}

		LoadedPlugin& p = *g_loadedPlugins[index];
		ModLoaderLogger::LogMessage(L"Unloading plugin: %S", p.cachedName.c_str());
		p.shutdown();
		p.isInitialized = false;
		FreeLibrary(p.hModule);
		p.hModule = nullptr;
		p.info    = nullptr;

		LeaveCriticalSection(&g_pluginLock);
		ModLoaderLogger::LogMessage(L"Plugin unloaded: %S", p.cachedName.c_str());
		return true;
	}

	bool ReloadPlugin(int index)
	{
		EnterCriticalSection(&g_pluginLock);

		if (index < 0 || index >= static_cast<int>(g_loadedPlugins.size()))
		{
			LeaveCriticalSection(&g_pluginLock);
			return false;
		}

		LoadedPlugin& p = *g_loadedPlugins[index];
		ModLoaderLogger::LogMessage(L"Reloading plugin: %S", p.cachedName.c_str());

		if (p.isInitialized)
		{
			p.shutdown();
			p.isInitialized = false;
		}
		if (p.hModule)
		{
			FreeLibrary(p.hModule);
			p.hModule = nullptr;
			p.info    = nullptr;
		}

		bool ok = LoadPluginIntoRecord(p);

		LeaveCriticalSection(&g_pluginLock);

		if (ok)
		{
			RefreshSelfPointers(p);
			InitAllLoadedPlugins();
			ModLoaderLogger::LogMessage(L"Plugin reloaded and initialized: %S", p.cachedName.c_str());
		}
		else
		{
			ModLoaderLogger::LogMessage(L"Plugin reload failed for index %d", index);
		}

		return ok;
	}

	HANDLE GetPluginsInitializedEvent()
	{
		return g_initCompleteEvent;
	}

	bool IsStartupComplete()
	{
		return g_startupComplete;
	}

	void MarkStartupComplete()
	{
		g_startupComplete = true;
		ModLoaderLogger::LogMessage(L"Plugin startup phase complete");
	}
}
