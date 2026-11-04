#include "plugin_manager.h"

#include <atomic>
#include <memory>

#include "plugin_interface.h"
#include "logging/log.h"
#include "logging/logger.h"
#include "logging/logger_interface.h"
#include "config/config_manager.h"
#include "memory_scanner/hook_scanner_interface.h"
#include "plugins/plugin_hook_report.h"
#include "plugins/plugin_guard.h"
#include "auto_update/auto_updater.h"
#include "plugins/plugin_watchdog.h"
#include "utils/game_thread_dispatch.h"
#include "plugins/reload_policy.h"
#include "plugins/crash_restart.h"
#include "hooks/hooks_interface.h"
#include "memory_scanner/version_map.h"
#include "console/plugin_console.h"
#include "plugins/pak_registry.h"
#include "preload/preload_manager.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/game/game_menu/game_menu_registry.h"
#include "hooks/input/mouse_wheel_registry.h"
#include "core/ui_state.h"
#endif
#include <vector>
#include <string>
#include <cstring>
#include <atomic>
#include <mutex>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

// Build tag is set by CI; fall back to a local placeholder.
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/splash_window.h"
#include "UI/modloader_window.h"
#include "UI/plugin_panel_registry.h"
#include "UI/plugin_widget_registry.h"
#include "UI/global_settings.h"
#include "UI/toast.h"
#endif

namespace PluginManager
{
	// SEH helpers for catching crashes inside plugin-supplied functions.
	// Must be plain POD functions with no C++ objects (C2712).
	struct PluginCrashCtx { DWORD code; uintptr_t addr; CONTEXT context; };
	static PluginCrashCtx g_lastPluginCrash;

	// Set once by RefusePluginLoads on the init thread; read wherever a load starts.
	static std::atomic<bool> g_refuseLoads{ false };

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
	// dbghelp is single-threaded, and a guarded callback can crash on the
	// render thread while the game thread logs another, so one walk at a time.
	static void AppendStackTrace(const CONTEXT& context, wchar_t* outBuffer, size_t outBufferLen)
	{
		static std::mutex s_dbghelpLock;
		std::lock_guard<std::mutex> lock(s_dbghelpLock);
		EnsureSymInitialized();

		CONTEXT ctx = context;
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
		AppendStackTrace(g_lastPluginCrash.context, stackTrace, std::size(stackTrace));

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

	// Shutdown also runs after a plugin crashed, and a crash can leave the
	// plugin's own locks held, so a shutdown that hangs is named in the log.
	static bool CallShutdown(PluginShutdownFunc fn)
	{
		PluginWatchdog::Scope watch(reinterpret_cast<const void*>(fn), "PluginShutdown");
		return CallShutdownSEH(fn);
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

	static bool CallLoadHooksSEH(PluginLoadHooksFunc fn, IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_lastPluginCrash = {};
		__try { fn(self, scanner); return true; }
		__except (PluginCrashFilter(GetExceptionInformation())) { return false; }
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
		AppendStackTrace(g_lastPluginCrash.context, stackTrace, std::size(stackTrace));

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


	// PluginGuard's reporter: one of a running plugin's callbacks crashed, and
	// the guard has switched the plugin off. Called once per plugin load, on the
	// thread that crashed. Takes no plugin-manager lock: the render thread can
	// get here while the game thread holds g_pluginLock and waits for it.
	static void ReportGuardedCrash(const PluginGuard::Report& r)
	{
		const uintptr_t base   = reinterpret_cast<uintptr_t>(r.module);
		const uintptr_t offset = r.faultAddress >= base ? r.faultAddress - base : 0;

		wchar_t faultModuleName[MAX_PATH];
		uintptr_t faultOffset = 0;
		ResolveModuleAndOffset(r.faultAddress, faultModuleName, MAX_PATH, &faultOffset);

		// The guard walked the stack before the unwind, so every frame is real.
		wchar_t stackTrace[2048] = {};
		for (size_t i = 0; i < r.frameCount; ++i)
		{
			wchar_t modName[MAX_PATH];
			uintptr_t modOffset = 0;
			ResolveModuleAndOffset(r.frames[i], modName, MAX_PATH, &modOffset);

			wchar_t line[MAX_PATH + 32];
			swprintf_s(line, L"\n    #%zu  %s+0x%llX", i, modName, static_cast<unsigned long long>(modOffset));
			wcscat_s(stackTrace, line);
		}

		ModLoaderLogger::LogError(
			L"[PluginManager] Plugin '%S' CRASHED in its %S -- code=0x%08X  fault=0x%llX  (+0x%llX from plugin DLL base)"
			L"  faultModule=%s+0x%llX  stack:%s",
			r.pluginName, r.callback, r.code,
			static_cast<unsigned long long>(r.faultAddress),
			static_cast<unsigned long long>(offset),
			faultModuleName,
			static_cast<unsigned long long>(faultOffset),
			stackTrace);
		ModLoaderLogger::LogError(
			L"[PluginManager] '%S' is switched off: the loader calls none of its callbacks until it is unloaded or reloaded. "
			L"Detours it placed on game functions keep running.",
			r.pluginName);

		// Only noted here, on whatever thread faulted: PumpCrashRestarts decides
		// what happens next on the game thread.
		CrashRestart::Global().NoteCrash(reinterpret_cast<uintptr_t>(r.module), r.pluginName, r.callback, GetTickCount64());
	}

	// Structure to hold loaded plugin information
	struct LoadedPlugin
	{
		HMODULE hModule;
		PluginInfo* info;
		VersionMap::PluginGameBuild gameBuild;   // the PluginInfo tail, copied at load: info dies with the DLL
		ReloadPolicy::Settings reload;           // its load and unload settings, copied the same way
		GetPluginInfoFunc getInfo;
		PluginInitFunc init;
		PluginShutdownFunc shutdown;
		PluginLoadHooksFunc loadHooks;   // optional export, may be null
		std::wstring fileName;
		bool isInitialized;

		// Cached display strings — valid even after FreeLibrary.
		std::string cachedName;
		std::string cachedVersion;
		std::string cachedAuthor;

		bool isOutOfDate;            // true when it may not run here: a newer interface, or another game build
		bool needsModLoaderUpdate;   // true when plugin interface version is too new
		bool isWrongTarget;          // true when plugin was built for a different build target
		bool hookScanFailed;         // true when OnPluginLoadHooks missed anything at all

		// Stable identity struct passed to PluginInit and retained by the plugin->
		// name/version point into cachedName/cachedVersion so they outlive PluginInfo.
		IPluginSelf self;
	};

	std::vector<std::unique_ptr<LoadedPlugin>> g_loadedPlugins;
	static CRITICAL_SECTION g_pluginLock;
	static bool g_managerInitialized = false;
	static bool   g_startupComplete    = false;

	// Bumped whenever a plugin is loaded, unloaded or reloaded.
	//
	// Exists so UI that caches anything derived from a plugin -- the config editor
	// caches the parsed contents of its .ini -- can tell that its cache is stale
	// without every such site having to be wired up to a notification. The config
	// editor previously rebuilt only when the *selected plugin index* changed, so
	// reloading the selected plugin left it showing the schema and values from the
	// first time it was ever opened, and a newly added setting was invisible until
	// you clicked onto another plugin and back.
	static unsigned g_pluginGeneration = 0;
	static HANDLE g_initCompleteEvent  = NULL;

	// Update self pointers after loading or reloading a plugin, so they point to the cached strings.
	static void RefreshSelfPointers(LoadedPlugin& rec)
	{
		rec.self.name = rec.cachedName.c_str();
		rec.self.version = rec.cachedVersion.c_str();
	}

	// Why a DLL did not come up, so ScanForNewPlugins can say so. Most failures
	// leave no record behind, so the reason has to travel out separately.
	enum class LoadFailure { None, CouldNotLoad, Crashed, NotAPlugin, OutOfDate, WrongTarget, OffBuild, Refused };

	struct LoadFailureInfo
	{
		LoadFailure kind             = LoadFailure::None;
		DWORD       win32Error       = 0;   // CouldNotLoad: GetLastError() of LoadLibrary
		int         interfaceVersion = 0;   // OutOfDate: the newer interface the plugin was built for
		std::string name;                   // whatever name the DLL reported, if it got that far
		std::string detail;                 // OffBuild: VersionMap::OffBuildRefusal's or LayoutRefusal's reason; Refused: the load setting's
	};

	static const wchar_t* BaseFileName(const std::wstring& path);
	static bool HasUpdateSidecar(const std::wstring& dllPath);

	// Inner load helper: performs LoadLibrary, GetProcAddress, GetPluginInfo,
	// version check, and PluginInit on an existing LoadedPlugin record.
	// rec.fileName must be set before calling. On success: hModule, info,
	// function pointers, cached strings, and isInitialized are all populated.
	// On failure: any resources acquired are released and the record is left clean,
	// and why (if given) says which way it failed.
	//
	// startup: this is the loader's startup plugin load. Anywhere else the plugin's
	// load setting is checked once its info has been read (a DLL has to be loaded
	// to be asked), and a refused plugin is freed before PluginInit or its load
	// hooks run.
	static bool LoadPluginIntoRecord(LoadedPlugin& rec, bool startup, LoadFailureInfo* why = nullptr)
	{
		if (g_refuseLoads.load())
		{
			ModLoaderLogger::LogWarn(L"Plugin loading is off (the game's layout does not match this ModLoader) -- skipping: %s",
				rec.fileName.c_str());
			return false;
		}

		ModLoaderLogger::LogMessage(L"Loading plugin: %s", rec.fileName.c_str());

		LoadLibrarySEHResult loadResult = LoadLibrarySEH(rec.fileName.c_str());
		if (loadResult.crashed)
		{
			LogPluginLoadCrash(rec.fileName.c_str(), L"LoadLibrary (plugin DllMain)");
			ModLoaderLogger::LogError(L"Plugin DLL crashed while loading -- skipping: %s", rec.fileName.c_str());
			if (why) why->kind = LoadFailure::Crashed;
			return false;
		}

		HMODULE hModule = loadResult.hModule;
		if (!hModule)
		{
			DWORD error = GetLastError();
			ModLoaderLogger::LogMessage(L"Failed to load plugin DLL: %s (error: %lu)", rec.fileName.c_str(), error);
			if (why) { why->kind = LoadFailure::CouldNotLoad; why->win32Error = error; }
			return false;
		}

		GetPluginInfoFunc getInfo = reinterpret_cast<GetPluginInfoFunc>(
			GetProcAddress(hModule, PLUGIN_GET_INFO_FUNC_NAME));
		PluginInitFunc init = reinterpret_cast<PluginInitFunc>(
			GetProcAddress(hModule, PLUGIN_INIT_FUNC_NAME));
		PluginShutdownFunc shutdown = reinterpret_cast<PluginShutdownFunc>(
			GetProcAddress(hModule, PLUGIN_SHUTDOWN_FUNC_NAME));

		// Optional fourth export -- plugins that resolve no AOB patterns do not
		// have one, and its absence is not an error.
		PluginLoadHooksFunc loadHooks = reinterpret_cast<PluginLoadHooksFunc>(
			GetProcAddress(hModule, PLUGIN_LOAD_HOOKS_FUNC_NAME));

		if (!getInfo || !init || !shutdown)
		{
			ModLoaderLogger::LogMessage(L"Plugin missing required exports: %s", rec.fileName.c_str());
			FreeLibrary(hModule);
			if (why) why->kind = LoadFailure::NotAPlugin;
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
			if (why) why->kind = LoadFailure::Crashed;
			return false;
		}

		// What is installed, for the plugin update pass, whether or not it
		// loads: a build put in by hand is never replaced by an older release.
		if (HasUpdateSidecar(rec.fileName))
			NoteInstalledPluginVersion(BaseFileName(rec.fileName), info->version);

		// Only a newer interface is refused here: the tables only grow, so an
		// older plugin gets the slots it knows. Whether it may run on this game
		// build is the game-build check below, before anything else is read.
		if (info->interfaceVersion > PLUGIN_INTERFACE_VERSION_MAX)
		{
			ModLoaderLogger::LogMessage(L"Plugin interface version %d is newer than this loader supports (up to %d): %s",
				info->interfaceVersion, PLUGIN_INTERFACE_VERSION_MAX, rec.fileName.c_str());
			// Cache what we can from the DLL so the UI can display it.
			rec.cachedName          = info->name    ? info->name    : "";
			rec.cachedVersion       = info->version ? info->version : "";
			rec.cachedAuthor        = info->author  ? info->author  : "";
			rec.isOutOfDate         = true;
			rec.needsModLoaderUpdate = true;
			// info points into the plugin's own image, so read it before the free.
			if (why) { why->kind = LoadFailure::OutOfDate; why->interfaceVersion = info->interfaceVersion; }
			FreeLibrary(hModule);
			return false;
		}

		// A plugin is judged against the build its own SDK came from. Off that
		// build it reads the game where its build had things, unless it takes
		// everything from the map. The globals are asked for after PluginInit.
		// It reads only interfaceVersion and the v72 tail, so it runs before
		// pluginTarget, which a very old PluginInfo does not have.
		rec.gameBuild = VersionMap::ReadPluginGameBuild(*info);
		if (info->interfaceVersion >= 72 && !rec.gameBuild.declared)
			ModLoaderLogger::LogWarn(L"Plugin '%S' declares no game build; judging it as built on CL-%u.",
				info->name ? info->name : "(unknown)", rec.gameBuild.gameCL);
		// The same holds on its own build when the layout check found classes
		// changed: only a plugin that takes every layout from the map runs.
		std::string refusal = VersionMap::OffBuildRefusal(VersionMap::RunningExe(), info->interfaceVersion, rec.gameBuild, true);
		if (refusal.empty())
			refusal = VersionMap::LayoutRefusal(info->interfaceVersion, rec.gameBuild);
		if (!refusal.empty())
		{
			ModLoaderLogger::LogWarn(L"Plugin '%S' is not loaded: %S", info->name ? info->name : "(unknown)", refusal.c_str());
			rec.cachedName           = info->name    ? info->name    : "";
			rec.cachedVersion        = info->version ? info->version : "";
			rec.cachedAuthor         = info->author  ? info->author  : "";
			rec.isOutOfDate          = true;
			rec.needsModLoaderUpdate = false;
			if (why) { why->kind = LoadFailure::OffBuild; why->detail = refusal; }
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
			if (why) why->kind = LoadFailure::WrongTarget;
			return false;
		}

		rec.reload = ReloadPolicy::Read(info->interfaceVersion, rec.gameBuild.mapFlags);
		if (const std::string refusal = ReloadPolicy::LoadRefusal(rec.reload.load, startup, ReloadPolicy::GameplayWorldRunning());
			!refusal.empty())
		{
			ModLoaderLogger::LogWarn(L"Plugin '%S' is not loaded: %S", info->name ? info->name : "(unknown)", refusal.c_str());
			rec.cachedName    = info->name    ? info->name    : "";
			rec.cachedVersion = info->version ? info->version : "";
			rec.cachedAuthor  = info->author  ? info->author  : "";
			if (why) { why->kind = LoadFailure::Refused; why->detail = refusal; }
			FreeLibrary(hModule);
			return false;
		}

		ModLoaderLogger::LogMessage(L"Plugin info - Name: %S, Version: %S, Author: %S, Interface: %d (modloader expects [%d, %d]), game build %S",
			info->name, info->version, info->author,
			info->interfaceVersion, PLUGIN_INTERFACE_VERSION_MIN, PLUGIN_INTERFACE_VERSION_MAX,
			VersionMap::DescribePluginGameBuild(rec.gameBuild).c_str());

		rec.hModule        = hModule;
		rec.info           = info;
		rec.getInfo        = getInfo;
		rec.init           = init;
		rec.shutdown       = shutdown;
		rec.loadHooks      = loadHooks;
		rec.isInitialized  = false;  // deferred -- InitAllLoadedPlugins() calls PluginInit
		rec.cachedName     = info->name    ? info->name    : "";
		rec.cachedVersion  = info->version ? info->version : "";
		rec.cachedAuthor   = info->author  ? info->author  : "";

		ModLoaderLogger::LogMessage(L"Successfully loaded plugin DLL: %S v%S (PluginInit deferred)", info->name, info->version);
		return true;
	}

	// Load a single plugin DLL by path (the startup scan, and ScanForNewPlugins).
	static bool LoadPlugin(const std::wstring& dllPath, bool startup, LoadFailureInfo* why = nullptr)
	{
		auto rec = std::make_unique<LoadedPlugin>();
		rec->fileName = dllPath;
		rec->isOutOfDate         = false;
		rec->needsModLoaderUpdate = false;
		rec->isWrongTarget        = false;
		rec->hookScanFailed       = false;

		if (!LoadPluginIntoRecord(*rec, startup, why))
		{
			if (why) why->name = rec->cachedName;

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
		PluginGuard::SetReporter(ReportGuardedCrash);
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

	// Enumerate every .dll in ModLoader\Plugins, creating the directory if it is
	// missing. Shared by the startup scan and the console's runtime rescan.
	// Returns false only if the directory does not exist and could not be created.
	static bool CollectPluginDllPaths(std::vector<std::wstring>& outPaths)
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		wchar_t* lastSlash = wcsrchr(exePath, L'\\');
		if (lastSlash) *lastSlash = L'\0';

		wchar_t modsPath[MAX_PATH] = {};
		swprintf_s(modsPath, L"%s\\ModLoader\\Plugins", exePath);

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
				return false;
			}
		}

		wchar_t searchPattern[MAX_PATH] = {};
		swprintf_s(searchPattern, L"%s\\*.dll", modsPath);

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
					outPaths.push_back(dllPath);
				}
			} while (FindNextFileW(hFind, &findData));
			FindClose(hFind);
		}

		return true;
	}

	// Bare file name of a plugin record's path, e.g. "ServerUtility.dll".
	static const wchar_t* BaseFileName(const std::wstring& path)
	{
		const wchar_t* slash = wcsrchr(path.c_str(), L'\\');
		return slash ? slash + 1 : path.c_str();
	}

	// True when the auto-update sidecar for this DLL exists on disk:
	// "...\Plugins\MyPlugin.dll" -> "...\Plugins\MyPlugin.json". Matches the
	// pairing the updater's per-plugin pass uses (auto_update/auto_updater.cpp),
	// which is the only thing that makes a plugin able to update itself.
	//
	// Only the file's presence is checked, not its contents: a sidecar with a
	// missing or unreachable manifest_url is the updater's problem to report,
	// and re-parsing every sidecar on every UI frame to say so would not be.
	static bool HasUpdateSidecar(const std::wstring& dllPath)
	{
		const size_t dot = dllPath.find_last_of(L'.');
		const size_t slash = dllPath.find_last_of(L'\\');
		if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
			return false;

		std::wstring sidecar = dllPath.substr(0, dot) + L".json";
		const DWORD attrs = GetFileAttributesW(sidecar.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	void LoadAllPlugins()
	{
		if (!g_managerInitialized)
		{
			ModLoaderLogger::LogMessage(L"ERROR: Plugin manager not initialized");
			return;
		}

		ModLoaderLogger::LogMessage(L"Searching for plugins in ModLoader\\Plugins");

		// Pre-collect paths so we know the total count for progress reporting.
		std::vector<std::wstring> dllPaths;
		if (!CollectPluginDllPaths(dllPaths))
			return;

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

			if (LoadPlugin(dllPaths[i], true))
				loadedCount++;
		}

#ifdef MODLOADER_CLIENT_BUILD
		Splash::ClearSubBar();
#endif

		ModLoaderLogger::LogMessage(L"Loaded %d plugin DLL(s) from Plugins (PluginInit deferred)", loadedCount);
	}

	// Another record in the plugin list with rec's name (case-insensitive), or
	// null. loadedOnly: only a record whose DLL is still loaded counts, else
	// any record does, refused ones too. Caller must hold g_pluginLock.
	static const LoadedPlugin* OtherPluginNamed(const LoadedPlugin& rec, bool loadedOnly)
	{
		for (const auto& other : g_loadedPlugins)
		{
			if (other.get() != &rec && (!loadedOnly || other->hModule)
				&& _stricmp(other->cachedName.c_str(), rec.cachedName.c_str()) == 0)
				return other.get();
		}
		return nullptr;
	}

	// Runs the plugin's OnPluginLoadHooks event, if it has one, and reports
	// whether the plugin may proceed to PluginInit.
	//
	// This is the whole point of moving AOB scanning into an event: a plugin
	// declares the addresses it depends on in one place, before it has done
	// anything, so a pattern that no longer matches can be turned into "this
	// plugin does not load" instead of a plugin that loads and then detours
	// whatever now lives at the address it guessed. One miss is enough --
	// optional resolves are a label on the report, not a second verdict.
	//
	// self->hooks stays null for the duration -- the event resolves, PluginInit
	// installs. A plugin that registered a callback here would be left with a
	// pointer into a freed module the moment a later pattern missed.
	//
	// Caller must hold g_pluginLock.
	static bool RunLoadHooksPhase(LoadedPlugin& plugin)
	{
		if (!plugin.loadHooks)
			return true;

		ModLoaderLogger::LogInfo(L"[HookScan] Resolving patterns for: %S v%S",
			plugin.cachedName.c_str(), plugin.cachedVersion.c_str());

#ifdef MODLOADER_CLIENT_BUILD
		// Worth its own splash line: with a cold scan cache this phase is a full
		// module scan per pattern, so it is where a slow startup actually goes --
		// and "Initializing <plugin>" would be pointing at the wrong step.
		{
			wchar_t msg[128];
			swprintf_s(msg, L"Resolving hooks for %S", plugin.cachedName.c_str());
			Splash::SetSubStatus(msg);
		}
#endif

		const wchar_t* baseName = BaseFileName(plugin.fileName);
		char fileNameA[128]{};
		WideCharToMultiByte(CP_ACP, 0, baseName, -1, fileNameA, static_cast<int>(sizeof(fileNameA)), "?", nullptr);
		fileNameA[sizeof(fileNameA) - 1] = '\0';

		// The version map keys a plugin's own sites by its name, so a name two
		// loaded plugins share reads none (VersionMap::ResolveMapKey). A record
		// whose DLL was refused reads nothing, so it does not count.
		PluginHookReport::BeginSession(&plugin.self, plugin.cachedName.c_str(), fileNameA,
			/*preload*/ false, OtherPluginNamed(plugin, true) != nullptr);

		const bool survived = CallLoadHooksSEH(plugin.loadHooks, &plugin.self,
			ModLoaderLogger::GetPluginHookScanner());

		if (!survived)
		{
			LogPluginCrash(plugin.cachedName.c_str(), plugin.hModule, L"OnPluginLoadHooks");
			PluginHookReport::RecordSessionCrash(&plugin.self,
				"crashed while resolving its patterns -- see modloader.log for the fault address and stack");
		}

		bool refused = false;
		PluginHookReport::EndSession(&plugin.self, &refused);
		return !refused;
	}

	// Every entry point of a record whose module is no longer mapped. Clearing
	// them is what stops a later init pass from calling into it --
	// LoadPluginIntoRecord repopulates them if the user reloads it. The cached
	// name/version strings are ours and stay, so the plugin list can still say
	// what happened.
	static void ForgetEntryPoints(LoadedPlugin& plugin)
	{
		plugin.hModule   = nullptr;
		plugin.info      = nullptr;
		plugin.getInfo   = nullptr;
		plugin.init      = nullptr;
		plugin.shutdown  = nullptr;
		plugin.loadHooks = nullptr;
	}

	static bool ShutDownAndFree(LoadedPlugin& p, const wchar_t* action);
	static void ForgetRegistrations(LoadedPlugin&);

	// What the plugin says it is busy with. A plugin that crashed and was
	// switched off is busy with nothing: a reason it set before the crash
	// would otherwise refuse the unload or reload that recovers it.
	static std::string BusyReasonOf(const LoadedPlugin& p)
	{
		if (p.hModule && PluginGuard::GetFault(p.hModule, nullptr))
			return {};
		return ReloadPolicy::BusyReason(&p.self);
	}

	// Calls PluginInit on a single record that has been loaded but not yet initialized.
	// Caller must hold g_pluginLock. Returns true if the plugin is now initialized.
	static bool InitPluginRecord(LoadedPlugin& plugin)
	{
		plugin.self.logger  = ModLoaderLogger::GetPluginLogger();
		plugin.self.config  = ModLoaderLogger::GetPluginConfig();
		plugin.self.hooks   = nullptr;   // populated below, after the scan phase
		ReloadPolicy::SetBusyReason(&plugin.self, nullptr);   // a new instance starts not busy
		PluginGuard::Track(plugin.hModule, plugin.cachedName.c_str());   // its callbacks are guarded from here on

		const bool resolved = RunLoadHooksPhase(plugin);
		const int interfaceVersion = plugin.info ? plugin.info->interfaceVersion : 0;
		if (!resolved)
		{
			// Refused. Free the DLL so nothing of it is left mapped -- it never
			// got as far as registering anything, and leaving a half-committed
			// plugin resident is how a stale detour outlives its module.
			plugin.hookScanFailed = true;
			if (plugin.hModule)
			{
				PluginGuard::Untrack(plugin.hModule);
				FreeLibrary(plugin.hModule);
			}
			ForgetEntryPoints(plugin);
			ModLoaderLogger::LogError(
				L"Plugin '%S' was NOT loaded: one or more of its hooks could not be resolved.",
				plugin.cachedName.c_str());
			return false;
		}

		plugin.hookScanFailed = false;
		plugin.self.hooks     = ModLoaderLogger::GetPluginHooks();

		ModLoaderLogger::LogMessage(L"Calling PluginInit for: %S v%S", plugin.cachedName.c_str(), plugin.cachedVersion.c_str());

#ifdef MODLOADER_CLIENT_BUILD
		UI::PluginPanelRegistry::SetCurrentRegistrationPlugin(plugin.cachedName.c_str());
#endif

		const unsigned globalsCallsBefore = ModLoaderLogger::SdkGlobalsCallsOnThisThread();
		InitSEHResult initResult = CallInitSEH(plugin.init, &plugin.self);
		const bool askedSdkGlobals = ModLoaderLogger::SdkGlobalsCallsOnThisThread() != globalsCallsBefore;
		const std::string lateRefusal = VersionMap::OffBuildRefusal(VersionMap::RunningExe(), interfaceVersion,
			plugin.gameBuild, askedSdkGlobals);
		bool success = false;

		if (initResult.crashed)
		{
			LogPluginCrash(plugin.cachedName.c_str(), plugin.hModule, L"PluginInit");
			ModLoaderLogger::LogError(L"Plugin '%S' crashed during PluginInit -- it has been left unloaded", plugin.cachedName.c_str());
			// Whatever it registered before the crash would go on being called.
			ForgetRegistrations(plugin);
		}
		else if (initResult.retval && !lateRefusal.empty())
		{
			// It ran PluginInit on its SDK's own Offsets::. Take back whatever
			// it registered there before anything of it runs in the game.
			ModLoaderLogger::LogWarn(L"Plugin '%S' was NOT loaded: %S", plugin.cachedName.c_str(), lateRefusal.c_str());
			plugin.isInitialized = true;
			plugin.isOutOfDate   = true;
			if (ShutDownAndFree(plugin, L"Unload"))
				ForgetEntryPoints(plugin);
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

	// Called right after PluginShutdown, before any of the purges that follow
	// it. The render thread may still be inside one of the plugin's panels or
	// widgets, and code running there can register more of anything; waiting
	// it out first means nothing gets added behind those purges. The panel and
	// widget purges are repeated in their usual place for whatever the second
	// wait let through.
	//
	// Returns false if the overlay is still in the plugin's code after the
	// (bounded) wait. The purges still run, since the plugin is shut down, but
	// the module must stay mapped: freeing it would crash the render thread on
	// return.
	static bool WaitOutPluginRenders(HMODULE module, const char* name, const wchar_t* action)
	{
#ifdef MODLOADER_CLIENT_BUILD
		const bool panelsIdle  = UI::PluginPanelRegistry::ForgetModule(module);
		const bool widgetsIdle = UI::PluginWidgetRegistry::ForgetModule(module);
		if (panelsIdle && widgetsIdle)
			return true;
		ModLoaderLogger::LogError(L"%s of %S skipped: the overlay is still drawing its panel. "
		                          L"It is shut down but stays loaded; reload it to try again.",
		                          action, name);
		return false;
#else
		(void)module; (void)name; (void)action;
		return true;
#endif
	}

	// Everything a plugin registered that points into its module: called after
	// PluginShutdown (and WaitOutPluginRenders), before FreeLibrary. Whatever
	// the plugin did not unregister itself, or unregistered from the wrong
	// list, would otherwise be called into freed memory -- or, once another
	// DLL is mapped at the same address, into the middle of its code.
	//
	// This drops what is registered or queued; it does not wait for a call
	// already running. The game and render threads are not inside the plugin
	// here (RELOAD and UNLOAD run on the game thread, and WaitOutPluginRenders
	// has waited out the overlay), but an HTTP or network thread can be.
	static void ForgetRegistrations(LoadedPlugin& p)
	{
		// The schema the config manager cached lives inside this module.
		ModLoaderLogger::ForgetPluginSchema(p.cachedName.c_str());

		// A console command or a game-menu row this plugin registered is a
		// handler address inside the module about to be unmapped, and the
		// console front-ends and the game's own menu would go on listing it and
		// happily call it. Pak mounts are NOT unmounted here -- see
		// pak_registry.h -- but their callbacks into this module go.
		PluginConsole::ForgetPlugin(p.cachedName.c_str());
		PakRegistry::ForgetPlugin(p.cachedName.c_str());

		// Every game-event, tick, input, network and HTTP callback.
		ModLoaderLogger::ForgetPluginCallbacks(p.hModule);

		// Game-thread tasks it posted that have not run yet, including any in
		// the batch running right now behind this unload: each holds its
		// callback and a context pointer into the module or its heap.
		GameThreadDispatch::ForgetModule(p.hModule);
#ifdef MODLOADER_CLIENT_BUILD
		// A panel or widget it registered and did not unregister: the
		// registries keep its descriptor and renderFn, both inside the module.
		UI::PluginPanelRegistry::ForgetModule(p.hModule);
		UI::PluginWidgetRegistry::ForgetModule(p.hModule);
		GameMenu::Registry::ForgetPlugin(p.cachedName.c_str());
		Hooks::MouseWheel::ForgetModule(p.hModule);
		UIState::ForgetModule(p.hModule);
		// So is an OnConfigChanged callback. The slot (and with it &p.self) is
		// kept across an unload, so a callback the plugin did not unregister
		// itself would still match this plugin and be called into freed memory.
		UI::PluginPanelRegistry::ForgetConfigCallbacks(&p.self);
#endif
	}

	void UnloadAllPlugins()
	{
		ModLoaderLogger::LogMessage(L"Unloading all plugins...");

		// No crash restart may reload anything from here on.
		CrashRestart::Global().Stop();

		EnterCriticalSection(&g_pluginLock);

		for (auto& plugin : g_loadedPlugins)
		{
			if (plugin->isInitialized)
			{
				ModLoaderLogger::LogMessage(L"Shutting down plugin: %S", plugin->cachedName.c_str());
				if (!CallShutdown(plugin->shutdown))
					LogPluginCrash(plugin->cachedName.c_str(), plugin->hModule, L"PluginShutdown (unload all)");
				plugin->isInitialized = false;
			}
			const bool idle = WaitOutPluginRenders(plugin->hModule, plugin->cachedName.c_str(), L"Unload");

			// Before FreeLibrary: everything it registered points into the module.
			ForgetRegistrations(*plugin);

			if (!idle)
				continue;   // left mapped, not freed under the overlay

			if (plugin->hModule)
			{
				PluginGuard::Untrack(plugin->hModule);
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
				out[i].hookScanFailed        = p.hookScanFailed;

				// Plugin paths are ASCII in practice; anything else degrades to '?'
				// rather than failing the whole snapshot. Cleared first so a
				// conversion failure leaves an empty string, not a partial one.
				out[i].fileName[0] = '\0';
				const wchar_t* baseName = BaseFileName(p.fileName);
				WideCharToMultiByte(CP_ACP, 0, baseName, -1,
					out[i].fileName, static_cast<int>(sizeof(out[i].fileName)), "?", nullptr);
				out[i].fileName[sizeof(out[i].fileName) - 1] = '\0';

				out[i].hasUpdateManifest = HasUpdateSidecar(p.fileName);

				const bool world = ReloadPolicy::GameplayWorldRunning();
				const std::string busy = BusyReasonOf(p);
				const std::string unloadWhy = p.isInitialized ? ReloadPolicy::UnloadRefusal(p.reload.unload, busy, world) : std::string();
				const std::string reloadWhy = ReloadPolicy::ReloadRefusal(p.reload, busy, p.isInitialized, world);
				strncpy_s(out[i].unloadBlock, unloadWhy.c_str(), _TRUNCATE);
				strncpy_s(out[i].reloadBlock, reloadWhy.c_str(), _TRUNCATE);

				PluginGuard::Fault fault;
				out[i].isCrashed      = p.hModule && PluginGuard::GetFault(p.hModule, &fault);
				out[i].crashReason[0] = '\0';
				if (out[i].isCrashed)
					sprintf_s(out[i].crashReason, "0x%08X in %s", fault.code, fault.callback ? fault.callback : "a callback");
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

	// PluginShutdown, then everything the plugin registered that lives in its
	// module, then FreeLibrary. Caller holds g_pluginLock. False when one of
	// its renders is still running: it is shut down, but stays mapped.
	static bool ShutDownAndFree(LoadedPlugin& p, const wchar_t* action)
	{
		if (!CallShutdown(p.shutdown))
			LogPluginCrash(p.cachedName.c_str(), p.hModule, L"PluginShutdown (unload)");
		p.isInitialized = false;
		ReloadPolicy::SetBusyReason(&p.self, nullptr);
		const bool idle = WaitOutPluginRenders(p.hModule, p.cachedName.c_str(), action);

		// Before FreeLibrary, not after: the moment the module is unmapped, every
		// pointer into it is a read or a call into freed memory.
		ForgetRegistrations(p);

		if (!idle)
		{
			++g_pluginGeneration;   // it is shut down now
			return false;
		}

		PluginGuard::Untrack(p.hModule);
		FreeLibrary(p.hModule);
		p.hModule = nullptr;
		p.info    = nullptr;
		++g_pluginGeneration;
		return true;
	}

	bool UnloadPlugin(int index, std::string* refusal)
	{
		if (refusal) refusal->clear();
		EnterCriticalSection(&g_pluginLock);

		if (index < 0 || index >= static_cast<int>(g_loadedPlugins.size()) ||
			!g_loadedPlugins[index]->isInitialized)
		{
			LeaveCriticalSection(&g_pluginLock);
			return false;
		}

		LoadedPlugin& p = *g_loadedPlugins[index];

		// Everything is asked before anything is shut down, so a refusal leaves
		// the plugin running exactly as it was.
		if (std::string why = ReloadPolicy::UnloadRefusal(p.reload.unload, BusyReasonOf(p),
				ReloadPolicy::GameplayWorldRunning()); !why.empty())
		{
			ModLoaderLogger::LogWarn(L"Unload of %S refused: %S", p.cachedName.c_str(), why.c_str());
			LeaveCriticalSection(&g_pluginLock);
			if (refusal) *refusal = std::move(why);
			return false;
		}

		ModLoaderLogger::LogMessage(L"Unloading plugin: %S", p.cachedName.c_str());
		const bool freed = ShutDownAndFree(p, L"Unload");
		LeaveCriticalSection(&g_pluginLock);
		if (freed)
			ModLoaderLogger::LogMessage(L"Plugin unloaded: %S", p.cachedName.c_str());
		return freed;
	}

	bool ReloadPlugin(int index, std::string* refusal)
	{
		if (refusal) refusal->clear();
		EnterCriticalSection(&g_pluginLock);

		if (index < 0 || index >= static_cast<int>(g_loadedPlugins.size()))
		{
			LeaveCriticalSection(&g_pluginLock);
			return false;
		}

		LoadedPlugin& p = *g_loadedPlugins[index];

		// The unload half and the load half are both asked before the first of
		// them starts: a refused reload must not leave a plugin shut down.
		if (std::string why = ReloadPolicy::ReloadRefusal(p.reload, BusyReasonOf(p), p.isInitialized,
				ReloadPolicy::GameplayWorldRunning()); !why.empty())
		{
			ModLoaderLogger::LogWarn(L"Reload of %S refused: %S", p.cachedName.c_str(), why.c_str());
			LeaveCriticalSection(&g_pluginLock);
			if (refusal) *refusal = std::move(why);
			return false;
		}

		ModLoaderLogger::LogMessage(L"Reloading plugin: %S", p.cachedName.c_str());

		if (p.isInitialized)
		{
			if (!CallShutdown(p.shutdown))
				LogPluginCrash(p.cachedName.c_str(), p.hModule, L"PluginShutdown (reload)");
			p.isInitialized = false;
			ReloadPolicy::SetBusyReason(&p.self, nullptr);
		}
		if (p.hModule)
		{
			const bool idle = WaitOutPluginRenders(p.hModule, p.cachedName.c_str(), L"Reload");

			// Same reason as UnloadPlugin. InitPluginRecord below re-registers
			// whatever the new build asks for.
			ForgetRegistrations(p);

			if (!idle)
			{
				++g_pluginGeneration;   // it is shut down now
				LeaveCriticalSection(&g_pluginLock);
				return false;
			}

			PluginGuard::Untrack(p.hModule);
			FreeLibrary(p.hModule);
			p.hModule = nullptr;
			p.info    = nullptr;
		}

		p.isOutOfDate         = false;
		p.needsModLoaderUpdate = false;
		p.isWrongTarget        = false;
		p.hookScanFailed       = false;

		LoadFailureInfo failure;
		bool ok = LoadPluginIntoRecord(p, false, &failure);
		++g_pluginGeneration;

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
			// The pre-check read the settings of the build that was loaded. If
			// the DLL on disk now declares a stricter load setting, say so.
			if (refusal && failure.kind == LoadFailure::Refused) *refusal = failure.detail;
			ModLoaderLogger::LogMessage(L"Plugin reload failed for index %d", index);
		}

		return ok;
	}

	// ---- Restarting a plugin the crash guard switched off (plugins/crash_restart.h)

	static bool RestartCrashedEnabled()
	{
#ifdef MODLOADER_CLIENT_BUILD
		return UI::GlobalSettings::GetRestartCrashedPlugins();
#else
		return true;
#endif
	}

	static void ShowCrashToast(const std::string& name, const std::string& text)
	{
#ifdef MODLOADER_CLIENT_BUILD
		UI::Toast::Show(name, text);
#else
		(void)name; (void)text;
#endif
	}

	// The index of the record still running `module` in its crashed state, and
	// its settings; -1 once it was unloaded or reloaded (nothing to restart).
	static int FindCrashedRecord(uintptr_t module, ReloadPolicy::Settings* settings)
	{
		int found = -1;
		EnterCriticalSection(&g_pluginLock);
		for (size_t i = 0; i < g_loadedPlugins.size(); ++i)
		{
			const LoadedPlugin& p = *g_loadedPlugins[i];
			if (p.isInitialized && p.hModule && reinterpret_cast<uintptr_t>(p.hModule) == module &&
				PluginGuard::GetFault(p.hModule, nullptr))
			{
				found     = static_cast<int>(i);
				*settings = p.reload;
				break;
			}
		}
		LeaveCriticalSection(&g_pluginLock);
		return found;
	}

	static bool IsRecordInitialized(int index)
	{
		EnterCriticalSection(&g_pluginLock);
		const bool on = index >= 0 && index < static_cast<int>(g_loadedPlugins.size()) && g_loadedPlugins[index]->isInitialized;
		LeaveCriticalSection(&g_pluginLock);
		return on;
	}

	static void RestartCrashed(const CrashRestart::Entry& e, int index, int done)
	{
		CrashRestart::Global().CountRestart(e.name);
		ModLoaderLogger::LogMessage(L"[PluginManager] Auto-restart %d of %d: reloading '%S' after it crashed in its %S",
			done + 1, CrashRestart::kMaxRestarts, e.name.c_str(), e.callback.c_str());

		std::string why;
		if (ReloadPlugin(index, &why) && IsRecordInitialized(index))
		{
			ModLoaderLogger::LogMessage(L"[PluginManager] '%S' was restarted", e.name.c_str());
			ShowCrashToast(e.name, CrashRestart::Restarted(e.name, e.callback));
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[PluginManager] Auto-restart of '%S' failed: %S", e.name.c_str(),
				why.empty() ? "it did not load or initialize again (see above)" : why.c_str());
			ShowCrashToast(e.name, CrashRestart::RestartFailed(e.name, e.callback, why));
		}
	}

	void PumpCrashRestarts()
	{
		CrashRestart::Scheduler& restarts = CrashRestart::Global();
		if (!restarts.HasWork() || !g_managerInitialized)
			return;

		const uint64_t now   = GetTickCount64();
		const bool     world = ReloadPolicy::GameplayWorldRunning();

		// A snapshot: a plugin that crashes again while one of these reloads it
		// is noted for a later tick, so nothing restarts twice in one frame.
		for (CrashRestart::Entry& e : restarts.TakeReady(now, world))
		{
			ReloadPolicy::Settings settings;
			const int index = FindCrashedRecord(e.module, &settings);
			if (index < 0)
			{
				ModLoaderLogger::LogMessage(L"[PluginManager] No auto-restart for '%S': it was unloaded or reloaded since it crashed",
					e.name.c_str());
				continue;
			}

			const int done = restarts.Restarts(e.name);
			const CrashRestart::Plan plan = CrashRestart::Decide(settings, world, done, RestartCrashedEnabled());
			const bool announce = e.stage == CrashRestart::Stage::New;

			switch (plan)
			{
			case CrashRestart::Plan::RestartNow:
				if (e.stage == CrashRestart::Stage::Scheduled)
				{
					if (!restarts.Stopped())
						RestartCrashed(e, index, done);
					break;
				}
				if (announce)
					ShowCrashToast(e.name, CrashRestart::Announcement(plan, e.name, e.callback));
				ModLoaderLogger::LogMessage(L"[PluginManager] '%S' will be restarted in %llu s (auto-restart %d of %d)",
					e.name.c_str(), static_cast<unsigned long long>(CrashRestart::kDelayMs / 1000), done + 1, CrashRestart::kMaxRestarts);
				restarts.Requeue(std::move(e), CrashRestart::Stage::Scheduled, now + CrashRestart::kDelayMs);
				break;

			case CrashRestart::Plan::WaitForMenu:
				if (announce)
					ShowCrashToast(e.name, CrashRestart::Announcement(plan, e.name, e.callback));
				ModLoaderLogger::LogMessage(L"[PluginManager] '%S' reloads only at the main menu: auto-restart waits for it",
					e.name.c_str());
				restarts.Requeue(std::move(e), CrashRestart::Stage::WaitingForMenu, 0);
				break;

			case CrashRestart::Plan::NeedsGameRestart:
				ShowCrashToast(e.name, CrashRestart::Announcement(plan, e.name, e.callback));
				ModLoaderLogger::LogMessage(L"[PluginManager] '%S' requires a game restart to load: it stays off until then",
					e.name.c_str());
				break;

			case CrashRestart::Plan::GaveUp:
				ShowCrashToast(e.name, CrashRestart::Announcement(plan, e.name, e.callback));
				ModLoaderLogger::LogWarn(L"[PluginManager] Giving up on '%S': it crashed again after %d automatic restarts and stays off until the game restarts",
					e.name.c_str(), done);
				break;

			case CrashRestart::Plan::Disabled:
				ShowCrashToast(e.name, CrashRestart::Announcement(plan, e.name, e.callback));
				ModLoaderLogger::LogMessage(L"[PluginManager] '%S' stays off: automatic restart is turned off in Settings",
					e.name.c_str());
				break;
			}
		}
	}

	int FindPluginIndex(const char* nameOrFile)
	{
		if (!nameOrFile || !*nameOrFile) return -1;

		// Widen once so the file-name comparisons can use _wcsicmp directly.
		wchar_t query[MAX_PATH] = {};
		MultiByteToWideChar(CP_ACP, 0, nameOrFile, -1, query, MAX_PATH);

		wchar_t queryDll[MAX_PATH] = {};
		swprintf_s(queryDll, L"%s.dll", query);

		EnterCriticalSection(&g_pluginLock);

		int found = -1;

		// PluginInfo name first -- that is what `plugins` displays and what a
		// user is most likely to type.
		for (int i = 0; i < static_cast<int>(g_loadedPlugins.size()); ++i)
		{
			if (_stricmp(g_loadedPlugins[i]->cachedName.c_str(), nameOrFile) == 0)
			{
				found = i;
				break;
			}
		}

		// Then the DLL file name, with and without the extension. A record whose
		// load failed has no cached name at all, so this is the only way to
		// address it.
		if (found < 0)
		{
			for (int i = 0; i < static_cast<int>(g_loadedPlugins.size()); ++i)
			{
				const wchar_t* baseName = BaseFileName(g_loadedPlugins[i]->fileName);
				if (_wcsicmp(baseName, query) == 0 || _wcsicmp(baseName, queryDll) == 0)
				{
					found = i;
					break;
				}
			}
		}

		LeaveCriticalSection(&g_pluginLock);
		return found;
	}

	HMODULE GetPluginModule(const char* nameOrFile)
	{
		// g_pluginLock is a critical section, so re-entering it through
		// FindPluginIndex is fine and keeps the index valid while we read it.
		EnterCriticalSection(&g_pluginLock);
		HMODULE mod = nullptr;
		const int index = FindPluginIndex(nameOrFile);
		if (index >= 0)
			mod = g_loadedPlugins[index]->hModule;
		LeaveCriticalSection(&g_pluginLock);
		return mod;
	}

	// Events NoteMissedEvent has been told about since the last reset. Only
	// ScanForNewPlugins reads it, around one plugin's PluginInit at a time.
	static std::atomic<unsigned> g_missedEvents{0};

	void NoteMissedEvent(MissedEvent event)
	{
		g_missedEvents |= event;
	}

	const char* ScanStatusLabel(ScanStatus status)
	{
		switch (status)
		{
		case ScanStatus::Live:         return "Loaded";
		case ScanStatus::Late:         return "Loaded late";
		case ScanStatus::NeedsRestart: return "Needs restart";
		case ScanStatus::Incompatible: return "Incompatible";
		case ScanStatus::WrongTarget:  return "Cannot load";
		case ScanStatus::HooksFailed:  return "Hooks failed";
		case ScanStatus::Retry:        return "Retry";
		case ScanStatus::NotAPlugin:   return "Not a plugin";
		case ScanStatus::Failed:       return "Failed";
		case ScanStatus::Duplicate:    return "Duplicate";
		case ScanStatus::Refused:      return "Refused";
		}
		return "?";
	}

	// Bare DLL file name, narrowed for display. Same conversion as the
	// fileName in GetAllPluginStatuses.
	static std::string BaseFileNameA(const std::wstring& path)
	{
		char buffer[128]{};
		WideCharToMultiByte(CP_ACP, 0, BaseFileName(path), -1, buffer, static_cast<int>(sizeof(buffer)), "?", nullptr);
		buffer[sizeof(buffer) - 1] = '\0';
		return buffer;
	}

	// "Registered for A and B after they had fired. <remedy>". All three events
	// fire again for every world (the hooks have no once-per-process guard), so
	// a save load catches the plugin up; the any-world event also fires when the
	// main menu world loads. A plugin that checks the current world itself
	// (BetterCheats, Codex and WAILA do) is already running, which registration
	// alone cannot show.
	static std::string LateAction(unsigned missed)
	{
		std::vector<const char*> events;
		if (missed & MissedChimeraMain)    events.push_back("the ChimeraMain world start");
		if (missed & MissedAnyWorld)       events.push_back("the any-world start");
		if (missed & MissedExperienceLoad) events.push_back("the experience load");

		std::string list;
		for (size_t i = 0; i < events.size(); ++i)
		{
			if (i > 0) list += (i + 1 == events.size()) ? " and " : ", ";
			list += events[i];
		}

		return "Registered for " + list + " after " + (events.size() == 1 ? "it" : "they") + " had fired. " +
			((missed & (MissedChimeraMain | MissedExperienceLoad))
				? "Load a save to catch up"
				: "Load a save or return to the main menu to catch up") +
			", unless the plugin checks the current world itself.";
	}

	// Result for a DLL LoadPlugin gave up on. The out-of-date and wrong-target
	// ones also keep a plugin-list row; the rest leave nothing behind.
	static ScanResult ResultForLoadFailure(const std::wstring& path, const LoadFailureInfo& why)
	{
		ScanResult r;
		r.fileName = BaseFileNameA(path);
		r.name     = why.name.empty() ? r.fileName : why.name;

		char text[256];
		switch (why.kind)
		{
		case LoadFailure::CouldNotLoad:
			// Only a file another process still holds is worth retrying; a missing
			// dependency or the wrong architecture will fail the same way next time.
			if (why.win32Error == ERROR_SHARING_VIOLATION || why.win32Error == ERROR_LOCK_VIOLATION ||
				why.win32Error == ERROR_ACCESS_DENIED)
			{
				r.status = ScanStatus::Retry;
				snprintf(text, sizeof(text),
					"Windows could not load it (error %lu). It may be locked or still being copied; try again in a moment.",
					why.win32Error);
			}
			else
			{
				r.status = ScanStatus::Failed;
				snprintf(text, sizeof(text),
					"Windows could not load it (error %lu). Check it is a 64-bit plugin and that its dependencies are present.",
					why.win32Error);
			}
			break;
		case LoadFailure::NotAPlugin:
			r.status = ScanStatus::NotAPlugin;
			snprintf(text, sizeof(text), "It has none of the plugin exports. Preload plugins go in ModLoader\\Preload.");
			break;
		case LoadFailure::OutOfDate:
			r.status = ScanStatus::Incompatible;
			snprintf(text, sizeof(text),
				"Built for plugin interface v%d, newer than this loader (%s accepts up to v%d). Update the mod loader.",
				why.interfaceVersion, MODLOADER_BUILD_TAG, PLUGIN_INTERFACE_VERSION_MAX);
			break;
		case LoadFailure::OffBuild:
			r.status = ScanStatus::Incompatible;
			snprintf(text, sizeof(text), "%s", why.detail.c_str());
			break;
		case LoadFailure::Refused:
			// Not a fault: the plugin asked not to be loaded in this state.
			r.status = ScanStatus::Refused;
			snprintf(text, sizeof(text), "%s", why.detail.c_str());
			break;
		case LoadFailure::WrongTarget:
			r.status = ScanStatus::WrongTarget;
			snprintf(text, sizeof(text), "Built for the other build target: a server plugin on the client, or the reverse.");
			break;
		default:
			r.status = ScanStatus::Failed;
			snprintf(text, sizeof(text), "It crashed while loading. See ModLoader.log.");
			break;
		}
		r.action = text;
		return r;
	}

	// Result for a record this scan appended and ran PluginInit on.
	static ScanResult ResultAfterInit(const LoadedPlugin& rec, bool inited, unsigned missedEvents)
	{
		ScanResult r;
		r.name     = rec.cachedName;
		r.fileName = BaseFileNameA(rec.fileName);

		if (inited && missedEvents == 0)
		{
			r.status = ScanStatus::Live;
			r.action = "Loaded. Nothing it registered in PluginInit had already fired.";
		}
		else if (inited)
		{
			r.status = ScanStatus::Late;
			r.action = LateAction(missedEvents);
		}
		else if (rec.hookScanFailed)
		{
			// Same wording as the plugin list row.
			r.status = ScanStatus::HooksFailed;
			r.action = "A hook it needs could not be found in this game build. It needs a new build from its author.";
		}
		else
		{
			r.status = ScanStatus::Failed;
			r.action = "PluginInit failed or crashed. See ModLoader.log.";
		}
		return r;
	}

	int ScanForNewPlugins(std::vector<ScanResult>* outResults)
	{
		if (!g_managerInitialized)
		{
			ModLoaderLogger::LogMessage(L"ERROR: Plugin manager not initialized");
			return 0;
		}

		std::vector<std::wstring> dllPaths;
		if (!CollectPluginDllPaths(dllPaths))
			return 0;

		// Held across the whole scan so nothing can append a record between the
		// "is this new?" test and the init pass below. CRITICAL_SECTION is
		// re-entrant on the owning thread, so LoadPlugin taking it again is fine.
		EnterCriticalSection(&g_pluginLock);

		const size_t firstNew = g_loadedPlugins.size();
		int loadedCount = 0;

		for (const std::wstring& path : dllPaths)
		{
			bool known = false;
			for (const auto& existing : g_loadedPlugins)
			{
				if (_wcsicmp(existing->fileName.c_str(), path.c_str()) == 0)
				{
					known = true;
					break;
				}
			}
			if (known)
				continue;

			ModLoaderLogger::LogMessage(L"ScanForNewPlugins: new plugin DLL found: %s", path.c_str());
			LoadFailureInfo why;
			if (!LoadPlugin(path, false, &why))
			{
				if (outResults)
					outResults->push_back(ResultForLoadFailure(path, why));
				continue;
			}

			// Plugin names key the config, console and panel registrations, so a
			// second DLL under a name that is already in use would trample the
			// first one's. LoadPlugin has just appended this record, so it is
			// the last one; everything before it is what it would collide with.
			// Any record counts, refused ones too: name lookups (FindPluginIndex,
			// config-change dispatch) return the first match, which a refused row
			// would otherwise keep.
			LoadedPlugin& added = *g_loadedPlugins.back();
			if (const LoadedPlugin* nameHolder = OtherPluginNamed(added, false))
			{
				ModLoaderLogger::LogError(
					L"ScanForNewPlugins: refusing %s -- a plugin named '%S' is already in the plugin list",
					path.c_str(), added.cachedName.c_str());
				if (outResults)
				{
					ScanResult r;
					r.status   = ScanStatus::Duplicate;
					r.name     = added.cachedName;
					r.fileName = BaseFileNameA(path);
					r.action   = "A plugin named '" + added.cachedName + "' is already in the plugin list (" +
						BaseFileNameA(nameHolder->fileName) + "). Remove or rename one of the two files.";
					outResults->push_back(std::move(r));
				}
				FreeLibrary(added.hModule);
				g_loadedPlugins.pop_back();
				continue;
			}

			loadedCount++;
		}

		// Init only the records this scan appended -- everything before firstNew
		// was either already initialized or deliberately left unloaded by the user.
		int initedCount = 0;
		for (size_t i = firstNew; i < g_loadedPlugins.size(); ++i)
		{
			LoadedPlugin& rec = *g_loadedPlugins[i];
			if (rec.isInitialized || !rec.init)
				continue;

			// The registrations in PluginInit report any one-time event they
			// arrive too late for; start from a clean slate for this plugin.
			g_missedEvents = 0;
			const bool inited = InitPluginRecord(rec);
			const unsigned missedEvents = g_missedEvents.exchange(0);
			if (inited)
				initedCount++;
			if (outResults)
				outResults->push_back(ResultAfterInit(rec, inited, missedEvents));
		}

		if (loadedCount > 0)
			++g_pluginGeneration;

		LeaveCriticalSection(&g_pluginLock);

		// Preload plugins run before the game starts, so a new one cannot be
		// loaded now. Say so rather than leave it looking ignored.
		for (const std::string& file : PreloadManager::FindNewDlls())
		{
			ModLoaderLogger::LogMessage(L"ScanForNewPlugins: new Preload DLL %S needs a game restart", file.c_str());
			if (outResults)
				outResults->push_back({ ScanStatus::NeedsRestart, file, file,
					"A Preload plugin runs before the game starts. Restart the game to load it." });
		}

		ModLoaderLogger::LogMessage(L"ScanForNewPlugins: %d new DLL(s) loaded, %d initialized", loadedCount, initedCount);
		return initedCount;
	}

	void RefusePluginLoads()
	{
		g_refuseLoads.store(true);
	}

	HANDLE GetPluginsInitializedEvent()
	{
		return g_initCompleteEvent;
	}

	bool IsStartupComplete()
	{
		return g_startupComplete;
	}

	unsigned GetPluginGeneration()
	{
		return g_pluginGeneration;
	}

	void MarkStartupComplete()
	{
		g_startupComplete = true;
		ModLoaderLogger::LogMessage(L"Plugin startup phase complete");
	}
}
