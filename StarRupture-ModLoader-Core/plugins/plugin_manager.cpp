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
#include <cstring>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/splash_window.h"
#include "UI/modloader_window.h"
#include "UI/plugin_panel_registry.h"
#endif

namespace PluginManager
{
	// SEH helpers for catching crashes inside plugin-supplied functions.
	// Must be plain POD functions with no C++ objects (C2712).
	struct PluginCrashCtx { DWORD code; uintptr_t addr; CONTEXT context; };
	static PluginCrashCtx g_lastPluginCrash;

	static LONG PluginCrashFilter(EXCEPTION_POINTERS* ep)
	{
		g_lastPluginCrash.code    = ep->ExceptionRecord->ExceptionCode;
		g_lastPluginCrash.addr    = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
		g_lastPluginCrash.context = *ep->ContextRecord;
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// Resolves an address to "<module filename>+0x<offset>". Returns "<unknown>+0x0"
	// if the address doesn't fall inside any loaded module.
	static void ResolveModuleAndOffset(uintptr_t addr, wchar_t* outName, size_t outNameLen, uintptr_t* outOffset)
	{
		wcscpy_s(outName, outNameLen, L"<unknown>");
		*outOffset = 0;

		HMODULE mod = nullptr;
		if (GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(addr),
				&mod) && mod)
		{
			wchar_t path[MAX_PATH];
			if (GetModuleFileNameW(mod, path, MAX_PATH))
			{
				// Keep just the filename, not the full path.
				const wchar_t* fileName = path;
				if (const wchar_t* slash = wcsrchr(path, L'\\'))
					fileName = slash + 1;
				wcscpy_s(outName, outNameLen, fileName);
			}
			*outOffset = addr - reinterpret_cast<uintptr_t>(mod);
		}
	}

	// Lazily initialise dbghelp's symbol handler so StackWalk64 can unwind across
	// module boundaries (it needs each module registered to look up its unwind
	// info via SymFunctionTableAccess64/SymGetModuleBase64). Refresh the module
	// list on subsequent calls in case plugins were loaded/unloaded since.
	static void EnsureSymInitialized()
	{
		static bool initialized = false;
		if (!initialized)
		{
			SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
			SymInitialize(GetCurrentProcess(), nullptr, TRUE);
			initialized = true;
		}
		else
		{
			SymRefreshModuleList(GetCurrentProcess());
		}
	}

	// Walks the call stack at the moment of the crash (captured CONTEXT) and
	// appends "  -> <module>+0x<offset>" entries to outBuffer, one per line.
	// No PDBs are shipped with Release builds, so frames are reported as
	// module+RVA -- match these against a local PDB/map file for the same build.
	static void AppendStackTrace(wchar_t* outBuffer, size_t outBufferLen)
	{
		EnsureSymInitialized();

		CONTEXT ctx = g_lastPluginCrash.context;
		STACKFRAME64 frame = {};
		frame.AddrPC.Offset    = ctx.Rip;
		frame.AddrPC.Mode      = AddrModeFlat;
		frame.AddrFrame.Offset = ctx.Rbp;
		frame.AddrFrame.Mode   = AddrModeFlat;
		frame.AddrStack.Offset = ctx.Rsp;
		frame.AddrStack.Mode   = AddrModeFlat;

		HANDLE process = GetCurrentProcess();
		HANDLE thread   = GetCurrentThread();

		static const int kMaxFrames = 24;
		for (int i = 0; i < kMaxFrames; ++i)
		{
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx,
					nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
				break;
			if (frame.AddrPC.Offset == 0)
				break;

			wchar_t modName[MAX_PATH];
			uintptr_t modOffset = 0;
			ResolveModuleAndOffset(static_cast<uintptr_t>(frame.AddrPC.Offset), modName, MAX_PATH, &modOffset);

			wchar_t line[MAX_PATH + 32];
			swprintf_s(line, L"\n    #%d  %s+0x%llX", i, modName, static_cast<unsigned long long>(modOffset));
			wcscat_s(outBuffer, outBufferLen, line);
		}
	}

	// Logs a caught crash, showing the fault RVA relative to the plugin's own DLL so the
	// plugin author can locate the crash in their own binary (e.g. with a map file or PDB).
	//
	// The fault may not actually be inside the plugin's own module -- it is often inside
	// a function-pointer call the plugin made into the modloader (or the game) itself.
	// To make that obvious, also resolve which module the fault address actually falls
	// in and report the RVA relative to that module's base, followed by a full
	// module+RVA call stack.
	static void LogPluginCrash(const char* pluginName, HMODULE hModule, const wchar_t* context)
	{
		uintptr_t base   = reinterpret_cast<uintptr_t>(hModule);
		uintptr_t offset = (base && g_lastPluginCrash.addr >= base) ? g_lastPluginCrash.addr - base : 0;

		wchar_t faultModuleName[MAX_PATH];
		uintptr_t faultOffset = 0;
		ResolveModuleAndOffset(g_lastPluginCrash.addr, faultModuleName, MAX_PATH, &faultOffset);

		wchar_t stackTrace[2048] = {};
		AppendStackTrace(stackTrace, std::size(stackTrace));

		ModLoaderLogger::LogError(
			L"[PluginManager] Plugin '%S' CRASHED during %s -- code=0x%08X  fault=0x%llX  (+0x%llX from plugin DLL base)"
			L"  faultModule=%s+0x%llX  stack:%s",
			pluginName, context,
			g_lastPluginCrash.code,
			static_cast<unsigned long long>(g_lastPluginCrash.addr),
			static_cast<unsigned long long>(offset),
			faultModuleName,
			static_cast<unsigned long long>(faultOffset),
			stackTrace);
	}

	static bool CallShutdownSEH(PluginShutdownFunc fn)
	{
		g_lastPluginCrash = {};
		__try { fn(); return true; }
		__except (PluginCrashFilter(GetExceptionInformation())) { return false; }
	}

	struct InitSEHResult { bool crashed; bool retval; };
	static InitSEHResult CallInitSEH(PluginInitFunc fn, IPluginSelf* self)
	{
		InitSEHResult r = {};
		g_lastPluginCrash = {};
		__try { r.retval = fn(self) != 0; }
		__except (PluginCrashFilter(GetExceptionInformation())) { r.crashed = true; }
		return r;
	}

	static PluginInfo* CallGetInfoSEH(GetPluginInfoFunc fn)
	{
		PluginInfo* result = nullptr;
		g_lastPluginCrash = {};
		__try { result = fn(); }
		__except (PluginCrashFilter(GetExceptionInformation())) {}
		return result;
	}

	struct LoadLibrarySEHResult { HMODULE hModule; bool crashed; };
	static LoadLibrarySEHResult LoadLibrarySEH(const wchar_t* path)
	{
		LoadLibrarySEHResult r = {};
		g_lastPluginCrash = {};
		__try { r.hModule = LoadLibraryW(path); }
		__except (PluginCrashFilter(GetExceptionInformation())) { r.crashed = true; }
		return r;
	}

	// Same as LogPluginCrash but for crashes caught before a plugin name/HMODULE
	// is known (e.g. a crash inside the plugin's own DllMain during LoadLibrary).
	static void LogPluginLoadCrash(const wchar_t* fileName, const wchar_t* context)
	{
		wchar_t faultModuleName[MAX_PATH];
		uintptr_t faultOffset = 0;
		ResolveModuleAndOffset(g_lastPluginCrash.addr, faultModuleName, MAX_PATH, &faultOffset);

		wchar_t stackTrace[2048] = {};
		AppendStackTrace(stackTrace, std::size(stackTrace));

		ModLoaderLogger::LogError(
			L"[PluginManager] Plugin DLL '%s' CRASHED during %s -- code=0x%08X  fault=0x%llX"
			L"  faultModule=%s+0x%llX  stack:%s",
			fileName, context,
			g_lastPluginCrash.code,
			static_cast<unsigned long long>(g_lastPluginCrash.addr),
			faultModuleName,
			static_cast<unsigned long long>(faultOffset),
			stackTrace);
	}


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

		bool isOutOfDate;            // true when plugin interface version is too old
		bool needsModLoaderUpdate;   // true when plugin interface version is too new
		bool isWrongTarget;          // true when plugin was built for a different build target

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

		LoadLibrarySEHResult loadResult = LoadLibrarySEH(rec.fileName.c_str());
		if (loadResult.crashed)
		{
			LogPluginLoadCrash(rec.fileName.c_str(), L"LoadLibrary (plugin DllMain)");
			ModLoaderLogger::LogError(L"Plugin DLL crashed while loading -- skipping: %s", rec.fileName.c_str());
			return false;
		}

		HMODULE hModule = loadResult.hModule;
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

		PluginInfo* info = CallGetInfoSEH(getInfo);
		if (!info)
		{
			if (g_lastPluginCrash.code)
			{
				uintptr_t base   = reinterpret_cast<uintptr_t>(hModule);
				uintptr_t offset = (base && g_lastPluginCrash.addr >= base) ? g_lastPluginCrash.addr - base : 0;
				ModLoaderLogger::LogError(
					L"[PluginManager] Plugin '%s' CRASHED in GetPluginInfo -- code=0x%08X  fault=0x%llX  (+0x%llX from plugin DLL base)",
					rec.fileName.c_str(), g_lastPluginCrash.code,
					static_cast<unsigned long long>(g_lastPluginCrash.addr),
					static_cast<unsigned long long>(offset));
			}
			else
			{
				ModLoaderLogger::LogMessage(L"Plugin GetPluginInfo returned nullptr: %s", rec.fileName.c_str());
			}
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
			rec.cachedName          = info->name    ? info->name    : "";
			rec.cachedVersion       = info->version ? info->version : "";
			rec.cachedAuthor        = info->author  ? info->author  : "";
			rec.isOutOfDate         = true;
			rec.needsModLoaderUpdate = (info->interfaceVersion > PLUGIN_INTERFACE_VERSION_MAX);
			FreeLibrary(hModule);
			return false;
		}

		rec.isWrongTarget = false;

#if defined(MODLOADER_CLIENT_BUILD)
		constexpr int currentTarget = PLUGIN_TARGET_CLIENT;
		constexpr const char* currentTargetName = "client";
		constexpr const char* expectedTargetName = "server";
#else
		constexpr int currentTarget = PLUGIN_TARGET_SERVER;
		constexpr const char* currentTargetName = "server";
		constexpr const char* expectedTargetName = "client";
#endif

		if (info->pluginTarget != currentTarget)
		{
			ModLoaderLogger::LogMessage(
				L"Plugin '%S' was built for a %S build but this is a %S build -- refusing to load.",
				info->name ? info->name : "(unknown)", expectedTargetName, currentTargetName);
			rec.cachedName    = info->name    ? info->name    : "";
			rec.cachedVersion = info->version ? info->version : "";
			rec.cachedAuthor  = info->author  ? info->author  : "";
			rec.isWrongTarget = true;
			FreeLibrary(hModule);
			return false;
		}

		ModLoaderLogger::LogMessage(L"Plugin info - Name: %S, Version: %S, Author: %S, Interface: %d (modloader expects [%d, %d])",
			info->name, info->version, info->author,
			info->interfaceVersion, PLUGIN_INTERFACE_VERSION_MIN, PLUGIN_INTERFACE_VERSION_MAX);

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
		rec->isOutOfDate         = false;
		rec->needsModLoaderUpdate = false;
		rec->isWrongTarget        = false;

		if (!LoadPluginIntoRecord(*rec))
		{
			// Keep the record visible in the UI for out-of-date or wrong-target plugins.
			if (rec->isOutOfDate || rec->isWrongTarget)
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
		swprintf_s(modsPath, L"%s\\ModLoader\\Plugins", exePath);

		ModLoaderLogger::LogMessage(L"Searching for plugins in: %s", modsPath);

		DWORD attribs = GetFileAttributesW(modsPath);
		if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
		{
			ModLoaderLogger::LogMessage(L"Plugins directory not found, creating it...");
			wchar_t modloaderPath[MAX_PATH]{};
			swprintf_s(modloaderPath, L"%s\\ModLoader", exePath);
			CreateDirectoryW(modloaderPath, nullptr);
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

	// Calls PluginInit on a single record that has been loaded but not yet initialized.
	// Caller must hold g_pluginLock. Returns true if the plugin is now initialized.
	static bool InitPluginRecord(LoadedPlugin& plugin)
	{
		ModLoaderLogger::LogMessage(L"Calling PluginInit for: %S v%S", plugin.cachedName.c_str(), plugin.cachedVersion.c_str());

		plugin.self.logger  = ModLoaderLogger::GetPluginLogger();
		plugin.self.config  = ModLoaderLogger::GetPluginConfig();
		plugin.self.scanner = ModLoaderLogger::GetPluginScanner();
		plugin.self.hooks   = ModLoaderLogger::GetPluginHooks();

#ifdef MODLOADER_CLIENT_BUILD
		UI::PluginPanelRegistry::SetCurrentRegistrationPlugin(plugin.cachedName.c_str());
#endif

		InitSEHResult initResult = CallInitSEH(plugin.init, &plugin.self);
		bool success = false;

		if (initResult.crashed)
		{
			LogPluginCrash(plugin.cachedName.c_str(), plugin.hModule, L"PluginInit");
			ModLoaderLogger::LogError(L"Plugin '%S' crashed during PluginInit -- it has been left unloaded", plugin.cachedName.c_str());
		}
		else if (initResult.retval)
		{
			plugin.isInitialized = true;
			success = true;
			ModLoaderLogger::LogMessage(L"Plugin initialized: %S", plugin.cachedName.c_str());
#ifdef MODLOADER_CLIENT_BUILD
			UI::ModLoaderWindow::LoadBlockingStateForPlugin(plugin.cachedName.c_str());
#endif
		}
		else
		{
			ModLoaderLogger::LogMessage(L"Plugin initialization failed: %S", plugin.cachedName.c_str());
		}

#ifdef MODLOADER_CLIENT_BUILD
		UI::PluginPanelRegistry::SetCurrentRegistrationPlugin(nullptr);
#endif
		return success;
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

#ifdef MODLOADER_CLIENT_BUILD
			{
				wchar_t msg[128];
				swprintf_s(msg, L"Initializing %S (%d/%d)", plugin->cachedName.c_str(), current, totalDeferred);
				Splash::SetSubStatus(msg);
				Splash::SetSubProgress(static_cast<float>(current - 1) / static_cast<float>(totalDeferred));
			}
#endif

			if (InitPluginRecord(*plugin))
				initCount++;
			else
				failCount++;
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
				if (!CallShutdownSEH(plugin->shutdown))
					LogPluginCrash(plugin->cachedName.c_str(), plugin->hModule, L"PluginShutdown (unload all)");
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
				out[i].isLoaded              = p.isInitialized;
				out[i].isOutOfDate           = p.isOutOfDate;
				out[i].needsModLoaderUpdate  = p.needsModLoaderUpdate;
				out[i].isWrongTarget         = p.isWrongTarget;
			}
		}
		LeaveCriticalSection(&g_pluginLock);
		return total;
	}

	const IPluginSelf* GetSelfForPlugin(const char* pluginName)
	{
		if (!pluginName) return nullptr;

		EnterCriticalSection(&g_pluginLock);
		const IPluginSelf* result = nullptr;
		for (auto& p : g_loadedPlugins)
		{
			if (_stricmp(p->cachedName.c_str(), pluginName) == 0)
			{
				result = &p->self;
				break;
			}
		}
		LeaveCriticalSection(&g_pluginLock);
		return result;
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
		if (!CallShutdownSEH(p.shutdown))
			LogPluginCrash(p.cachedName.c_str(), p.hModule, L"PluginShutdown (unload)");
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
			if (!CallShutdownSEH(p.shutdown))
				LogPluginCrash(p.cachedName.c_str(), p.hModule, L"PluginShutdown (reload)");
			p.isInitialized = false;
		}
		if (p.hModule)
		{
			FreeLibrary(p.hModule);
			p.hModule = nullptr;
			p.info    = nullptr;
		}

		p.isOutOfDate         = false;
		p.needsModLoaderUpdate = false;
		p.isWrongTarget        = false;

		bool ok = LoadPluginIntoRecord(p);

		if (ok)
		{
			RefreshSelfPointers(p);
			bool inited = InitPluginRecord(p);
			LeaveCriticalSection(&g_pluginLock);
			if (inited)
				ModLoaderLogger::LogMessage(L"Plugin reloaded and initialized: %S", p.cachedName.c_str());
			else
				ModLoaderLogger::LogMessage(L"Plugin reloaded but PluginInit failed: %S", p.cachedName.c_str());
		}
		else
		{
			LeaveCriticalSection(&g_pluginLock);
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
