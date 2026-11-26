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
#include "network_channel/network_channel.h"
#include "plugins/plugin_guard.h"
#include "plugins/plugin_host_info.h"
#include "plugins/plugin_version_query.h"
#include "plugins/declared_features_text.h"
#include "plugins/plugin_catalog.h"
#include "auto_update/auto_updater.h"
#include "auto_update/mod_index_fetcher.h"
#include "plugins/plugin_watchdog.h"
#include "utils/game_thread_dispatch.h"
#include "plugins/reload_policy.h"
#include "plugins/crash_restart.h"
#include "plugins/remote_reload.h"
#include "core/startup_utils.h"
#include "hooks/hooks_interface.h"
#include "memory_scanner/version_map.h"
#include "console/plugin_console.h"
#include "plugins/pak_registry.h"
#include "preload/preload_manager.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "auto_update/vortex_notice.h"
#include "auto_update/plugin_dll_version.h"
#include <algorithm>
#include <map>
#include <set>
#endif
#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/game/game_menu/game_menu_registry.h"
#include "hooks/input/keybind_registry.h"
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
#include "UI/image_library.h"
#include "UI/global_settings.h"
#include "UI/toast.h"
#endif

namespace PluginManager
{
	// SEH helpers for catching crashes inside plugin-supplied functions.
	// Must be plain POD functions with no C++ objects (C2712).
	struct PluginCrashCtx { DWORD code; uintptr_t addr; CONTEXT context; };
	// One shared slot, not locked: the calls that fill it (init, shutdown, the
	// load hooks) run one at a time on the game thread or during startup, except
	// after UnloadAllPlugins stops waiting for a PluginShutdown.
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
		PluginGuard::RunningScope running(reinterpret_cast<const void*>(fn));
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

	static const PluginWireInfo* CallGetWireInfoSEH(PluginWireInfoFunc fn)
	{
		const PluginWireInfo* result = nullptr;
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
		int interfaceVersion;                    // the interface it declares, copied the same way
		GetPluginInfoFunc getInfo;
		PluginInitFunc init;
		PluginShutdownFunc shutdown;
		PluginLoadHooksFunc loadHooks;   // optional export, may be null
		PluginSetHostInfoFunc setHostInfo;   // optional export, may be null
		WireCompat::Decl wire;           // what GetPluginWireInfo returned, copied at load; undeclared without the export
		std::wstring fileName;
		bool isInitialized;

		// True from the moment an unload or reload takes the plugin out of
		// service until PluginShutdown has returned and g_pluginLock is held again.
		// PluginShutdown runs in that window without the lock (see
		// ShutDownWithoutLock), so every other user of the record has to see
		// this. Only read or written with g_pluginLock held.
		bool isUnloading = false;

		// Cached display strings — valid even after FreeLibrary.
		std::string cachedName;
		std::string cachedVersion;
		std::string cachedAuthor;

		bool isOutOfDate;            // true when it may not run here: a newer interface, or another game build
		bool needsModLoaderUpdate;   // true when plugin interface version is too new
		bool isWrongTarget;          // true when plugin was built for a different build target
		bool hookScanFailed;         // true when OnPluginLoadHooks missed anything at all
		bool isDisabled;             // on [Plugins] Disabled when its load was last asked for
		bool isUninstalled = false;  // its files were moved to ModLoader\Removed

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
	// Set once InitAllLoadedPlugins has finished; GetDeclaredFeatures answers -1 before it.
	static std::atomic<bool> g_featuresReady{ false };

	// What the plugin list and the console say, in place of the buttons and as
	// the refusal, while a plugin is inside its PluginShutdown (isUnloading).
	static constexpr const char* kUnloadingNow = "This plugin is being unloaded right now.";

	// A record with no code, its plugin turned off or uninstalled. A
	// name lookup passes over it while another record has the name.
	static bool IsShelved(const LoadedPlugin& p)
	{
		return !p.hModule && (p.isDisabled || p.isUninstalled);
	}

	// The record a plugin name means: the first one that is not shelved, else
	// the first with the name, or -1. Caller must hold g_pluginLock.
	static int IndexByName(const char* name)
	{
		int shelved = -1;
		for (int i = 0; i < static_cast<int>(g_loadedPlugins.size()); ++i)
		{
			if (_stricmp(g_loadedPlugins[i]->cachedName.c_str(), name) != 0)
				continue;
			if (!IsShelved(*g_loadedPlugins[i]))
				return i;
			if (shelved < 0)
				shelved = i;
		}
		return shelved;
	}

	// Update self pointers after loading or reloading a plugin, so they point to the cached strings.
	static void RefreshSelfPointers(LoadedPlugin& rec)
	{
		rec.self.name = rec.cachedName.c_str();
		rec.self.version = rec.cachedVersion.c_str();
	}

	// Why a DLL did not come up, so ScanForNewPlugins can say so. Most failures
	// leave no record behind, so the reason has to travel out separately.
	enum class LoadFailure { None, CouldNotLoad, Crashed, NotAPlugin, OutOfDate, WrongTarget, OffBuild, Refused, Disabled, NeedsRestart };

	// What the console and a refusal say about a disabled plugin.
	static constexpr const char* kDisabledWhy = "disabled; enable it on the Browse page";
	static constexpr const char* kStaleWhy = "an uninstalled copy is still loaded; restart the game first";

	struct LoadFailureInfo
	{
		LoadFailure kind             = LoadFailure::None;
		DWORD       win32Error       = 0;   // CouldNotLoad: GetLastError() of LoadLibrary
		int         interfaceVersion = 0;   // OutOfDate: the newer interface the plugin was built for
		std::string name;                   // whatever name the DLL reported, if it got that far
		std::string detail;                 // OffBuild: VersionMap::OffBuildRefusal's or LayoutRefusal's reason; Refused: the load setting's
	};

	static const wchar_t* BaseFileName(const std::wstring& path);
	static int ScanPlugins(std::vector<ScanResult>* outResults, const std::wstring* only);
	static bool HasUpdateSidecar(const std::wstring& dllPath);
#ifdef MODLOADER_CLIENT_BUILD
	static int LoadReturnedCopies(std::vector<ScanResult>* outResults);
#endif

#ifdef MODLOADER_CLIENT_BUILD
	// Is this DLL on modloader.ini [Plugins] Disabled? Reads the file, so only
	// for a load that is being asked for, never per frame.
	static bool IsListedDisabled(const std::wstring& dllPath)
	{
		std::vector<std::wstring> disabled;
		PluginManage::ReadDisabledList(GetModLoaderDirPath(L"modloader.ini"), disabled);
		return PluginManage::ListHas(disabled, BaseFileName(dllPath));
	}

	static std::wstring LowerCase(std::wstring text)
	{
		if (!text.empty())
			CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
		return text;
	}

	// Paths uninstalled this session, each with the backup that holds the code
	// that may still be mapped from it, and DLLs that turned out to be no plugin.
	// Both are keyed by LowerCase path and guarded by g_pluginLock.
	static std::map<std::wstring, std::wstring> s_uninstalledPaths;
	static std::set<std::wstring> s_notPlugins;

	// A module uninstalled from this path is still mapped: LoadLibrary would
	// hand it back whatever file is there now.
	static bool IsStale(const std::wstring& path)
	{
		EnterCriticalSection(&g_pluginLock);
		const bool uninstalled = s_uninstalledPaths.count(LowerCase(path)) != 0;
		LeaveCriticalSection(&g_pluginLock);
		return uninstalled && GetModuleHandleW(path.c_str());
	}

	static void FillCachedIdentity(LoadedPlugin& rec);
#endif

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

#ifdef MODLOADER_CLIENT_BUILD
		// Before LoadLibrary: a disabled plugin runs none of its code. Every
		// load path comes through here.
		rec.isDisabled = IsListedDisabled(rec.fileName);
		if (rec.isDisabled)
		{
			ModLoaderLogger::LogMessage(L"Plugin is disabled -- not loading: %s", rec.fileName.c_str());
			FillCachedIdentity(rec);
			if (why) why->kind = LoadFailure::Disabled;
			return false;
		}
		if (IsStale(rec.fileName))
		{
			ModLoaderLogger::LogWarn(L"Load of %s refused: %S", rec.fileName.c_str(), kStaleWhy);
			if (why) why->kind = LoadFailure::NeedsRestart;
			return false;
		}
#endif

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

		// Optional fifth export: which other versions of the plugin its packets
		// are compatible with. No export means undeclared.
		PluginWireInfoFunc wireInfo = reinterpret_cast<PluginWireInfoFunc>(
			GetProcAddress(hModule, PLUGIN_WIRE_INFO_FUNC_NAME));

		// Optional sixth export: tells the plugin which interface level this loader has.
		PluginSetHostInfoFunc setHostInfo = reinterpret_cast<PluginSetHostInfoFunc>(
			GetProcAddress(hModule, PLUGIN_SET_HOST_INFO_FUNC_NAME));

		if (!getInfo || !init || !shutdown)
		{
			ModLoaderLogger::LogMessage(L"Plugin missing required exports: %s", rec.fileName.c_str());
			FreeLibrary(hModule);
#ifdef MODLOADER_CLIENT_BUILD
			EnterCriticalSection(&g_pluginLock);
			s_notPlugins.insert(LowerCase(rec.fileName));
			LeaveCriticalSection(&g_pluginLock);
#endif
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
			rec.interfaceVersion    = info->interfaceVersion;
			// The manifest is read before plugins load, so the latest release is
			// normally known here: say which one the plugin needs.
			LoaderManifest::Latest latest;
			if (GetLatestLoaderRelease(latest) && latest.HasRange())
			{
				char need[160];
				LoaderManifest::Describe(info->interfaceVersion, latest, need, sizeof(need));
				ModLoaderLogger::LogMessage(L"[AutoUpdate] %S (interface %d): %S",
					info->name ? info->name : "(unknown)", info->interfaceVersion, need);
			}
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
		// When the layout check found classes changed (the loader's SDK against
		// the game), a plugin runs only if it takes every layout from the map or
		// declares this very build.
		const VersionMap::Exe runningExe = VersionMap::RunningExe();
		std::string refusal = VersionMap::OffBuildRefusal(runningExe, info->interfaceVersion, rec.gameBuild, true);
		if (refusal.empty())
			refusal = VersionMap::LayoutRefusal(runningExe, info->interfaceVersion, rec.gameBuild);
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

		rec.interfaceVersion = info->interfaceVersion;
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
		rec.setHostInfo    = setHostInfo;
		rec.isInitialized  = false;  // deferred -- InitAllLoadedPlugins() calls PluginInit
		rec.cachedName     = info->name    ? info->name    : "";
		rec.cachedVersion  = info->version ? info->version : "";
		rec.cachedAuthor   = info->author  ? info->author  : "";

		// Copied now: the struct the plugin returns dies with its DLL.
		rec.wire = WireCompat::Decl{};
		if (wireInfo)
		{
			const PluginWireInfo* wi = CallGetWireInfoSEH(wireInfo);
			if (wi)
			{
				bool acceptRejected = false;
				rec.wire = WireCompat::MakeDecl(wi->wireId, wi->acceptVersions, &acceptRejected);
				if (acceptRejected)
					ModLoaderLogger::LogWarn(L"Plugin '%S' returned a compatible-versions list from GetPluginWireInfo that cannot be sent (too long or has control characters); it is ignored.",
						rec.cachedName.c_str());
			}
			else if (g_lastPluginCrash.code)
			{
				LogPluginCrash(rec.cachedName.c_str(), hModule, L"GetPluginWireInfo");
			}
		}

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
		rec->isDisabled           = false;

		if (!LoadPluginIntoRecord(*rec, startup, why))
		{
			if (why) why->name = rec->cachedName;

			// Keep the record visible in the UI for out-of-date, wrong-target or
			// disabled plugins.
			if (rec->isOutOfDate || rec->isWrongTarget || rec->isDisabled)
			{
				RefreshSelfPointers(*rec);
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
#ifdef MODLOADER_CLIENT_BUILD
			RefreshManagedFiles(); // there may be backups to restore
#endif
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
		RefreshManagedFiles();
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
				&& !IsShelved(*other) && !rec.cachedName.empty()
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

		PluginGuard::RunningScope running(reinterpret_cast<const void*>(plugin.loadHooks));
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
#ifdef MODLOADER_CLIENT_BUILD
		NetworkChannel::PluginFeaturesDeclared();
#endif
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
		plugin.setHostInfo = nullptr;
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
#ifdef MODLOADER_CLIENT_BUILD
		NetworkChannel::PluginFeaturesDeclared();   // joined rules may deny it: settle that before it registers anything
#endif

		// Tell the plugin which interface level this loader has, before it runs anything
		// of its own. A fault here is handled like a fault in PluginInit.
		if (plugin.setHostInfo)
		{
			g_lastPluginCrash = {};
			if (!PluginHostInfoCall::Run(plugin.setHostInfo, PluginCrashFilter))
			{
				LogPluginCrash(plugin.cachedName.c_str(), plugin.hModule, L"PluginSetHostInfo");
				ModLoaderLogger::LogError(L"Plugin '%S' crashed during PluginSetHostInfo -- it has been left unloaded", plugin.cachedName.c_str());
				ForgetRegistrations(plugin);
				return false;
			}
		}

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
		PluginGuard::RunningScope running(reinterpret_cast<const void*>(plugin.init));
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

	static void WriteDeclaredFeaturesDump();

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
			if (!plugin->isInitialized && !plugin->isUnloading && plugin->init) totalDeferred++;

		int initCount = 0;
		int failCount = 0;
		int current   = 0;

		for (auto& plugin : g_loadedPlugins)
		{
			if (plugin->isInitialized || plugin->isUnloading || !plugin->init)
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
		g_featuresReady = true;
		WriteDeclaredFeaturesDump();
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

		// The feature catalog holds the address of each `enabled` flag the
		// plugin declared, and those flags live inside this module.
		PluginHookReport::ForgetFeatures(&p.self);

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
		// The keys it asked the game not to see: a plugin that is gone must not
		// go on swallowing them. Loading it again applies its INI's Block flags.
		Hooks::Input::ForgetPluginBlocking(p.cachedName.c_str());
		// So is an OnConfigChanged callback. The slot (and with it &p.self) is
		// kept across an unload, so a callback the plugin did not unregister
		// itself would still match this plugin and be called into freed memory.
		UI::PluginPanelRegistry::ForgetConfigCallbacks(&p.self);

		// The image sets it said it draws from. Its images stay: they belong to
		// the session, not to the plugin.
		ImageLibrary::ForgetPlugin(&p.self);
#endif
	}

	// Caller holds g_pluginLock. True while some plugin is inside its
	// PluginShutdown on another thread (UnloadPlugin, ReloadPlugin): that thread
	// is still using the record, so the record must not be freed.
	static bool AnyPluginUnloading()
	{
		for (const auto& plugin : g_loadedPlugins)
			if (plugin->isUnloading)
				return true;
		return false;
	}

	void UnloadAllPlugins()
	{
		ModLoaderLogger::LogMessage(L"Unloading all plugins...");

		// No crash restart may reload anything from here on.
		CrashRestart::Global().Stop();

		EnterCriticalSection(&g_pluginLock);

		// An unload that is already inside a PluginShutdown finishes first. The
		// wait is bounded so a PluginShutdown that never returns cannot hold up
		// the exit as well; its record is then left alone below.
		const ULONGLONG unloadDeadline = GetTickCount64() + 5000;
		while (AnyPluginUnloading() && GetTickCount64() < unloadDeadline)
		{
			LeaveCriticalSection(&g_pluginLock);
			Sleep(1);
			EnterCriticalSection(&g_pluginLock);
		}
		for (const auto& plugin : g_loadedPlugins)
			if (plugin->isUnloading)
				ModLoaderLogger::LogError(L"%S is still inside its PluginShutdown after 5 s; continuing the shutdown without it",
					plugin->cachedName.c_str());

		for (auto& plugin : g_loadedPlugins)
		{
			if (plugin->isUnloading)
				continue;

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

		if (!AnyPluginUnloading())
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

	// A plugin that is running: initialised, not unloading and not switched off
	// by a crash. Caller holds g_pluginLock.
	static bool IsRunning(const LoadedPlugin& p)
	{
		return p.isInitialized && !p.isUnloading && !(p.hModule && PluginGuard::GetFault(p.hModule, nullptr));
	}

	bool GetPluginVersion(const char* name, char* out, size_t cap)
	{
		EnterCriticalSection(&g_pluginLock);
		const bool found = PluginVersionQuery::Find(g_loadedPlugins,
			[](const std::unique_ptr<LoadedPlugin>& p)
			{
				return PluginVersionQuery::Row{ &p->cachedName, &p->cachedVersion, IsRunning(*p) };
			},
			name, out, cap);
		LeaveCriticalSection(&g_pluginLock);
		return found;
	}

	// The declared-features text for the plugins running now. The feature catalog
	// is read before g_pluginLock is taken (it has its own lock).
	static std::string BuildDeclaredFeaturesText()
	{
		std::vector<DeclaredFeaturesText::FeatureRow> features;
		for (const PluginHookReport::FeatureInfo& f : PluginHookReport::Features())
			if (!f.preload)
				features.push_back({ f.plugin, f.id, f.title, !f.buildOff });

		std::vector<DeclaredFeaturesText::PluginRow> plugins;
		EnterCriticalSection(&g_pluginLock);
		for (const std::unique_ptr<LoadedPlugin>& p : g_loadedPlugins)
			if (IsRunning(*p))
				plugins.push_back({ p->cachedName, p->cachedVersion });
		LeaveCriticalSection(&g_pluginLock);

		return DeclaredFeaturesText::Build(VersionMap::RunningExe().cl, MODLOADER_BUILD_TAG, plugins, features);
	}

	int GetDeclaredFeatures(char* buf, size_t size)
	{
		if (!g_featuresReady)
			return -1;
		return DeclaredFeaturesText::CopyCut(BuildDeclaredFeaturesText(), buf, size);
	}

	int GetFeatureCatalog(char* buf, size_t size)
	{
#ifdef MODLOADER_CLIENT_BUILD
		return PluginCatalog::CopyAccessorText(buf, size);
#else
		// A server build does not fetch the catalog yet.
		(void)buf;
		(void)size;
		return -1;
#endif
	}

	// Best effort: a failure costs one warning and nothing else.
	static void WriteDeclaredFeaturesDump()
	{
		// One writer at a time: two reloads share the temp file's name.
		static std::mutex s_dumpMutex;
		std::lock_guard<std::mutex> lock(s_dumpMutex);

		const std::wstring dir = GetModLoaderDirPath(L"Logs");
		CreateDirectoryW(dir.c_str(), nullptr);
		const std::wstring path = dir + L"\\declared-features.v1.txt";
		if (!DeclaredFeaturesText::WriteAtomic(std::filesystem::path(path), BuildDeclaredFeaturesText()))
			ModLoaderLogger::LogWarn(L"Could not write %s", path.c_str());
	}

	const char* DescribeLoaderNeed(int declared, char* buf, size_t cap)
	{
		LoaderManifest::Latest latest;
		GetLatestLoaderRelease(latest);
		return LoaderManifest::Describe(declared, latest, buf, cap);
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
				out[i].isLoaded              = p.isInitialized || p.isUnloading;
				out[i].isOutOfDate           = p.isOutOfDate;
				out[i].needsModLoaderUpdate  = p.needsModLoaderUpdate;
				out[i].declaredInterface     = p.interfaceVersion;
				out[i].isWrongTarget         = p.isWrongTarget;
				out[i].hookScanFailed        = p.hookScanFailed;
				out[i].isDisabled            = p.isDisabled;
#ifdef MODLOADER_CLIENT_BUILD
				out[i].isRemoved             = p.isUninstalled;
#else
				out[i].isRemoved             = false;
#endif

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
				// A plugin in the middle of its PluginShutdown has no buttons:
				// the row says what is happening instead. Both blocks say it, as
				// reloadBlock also covers RELOAD of a loaded plugin.
				const std::string unloadWhy = p.isUnloading ? std::string(kUnloadingNow)
					: p.isInitialized ? ReloadPolicy::UnloadRefusal(p.reload.unload, busy, world) : std::string();
				const std::string reloadWhy = p.isUnloading ? std::string(kUnloadingNow)
					: ReloadPolicy::ReloadRefusal(p.reload, busy, p.isInitialized, world);
				strncpy_s(out[i].unloadBlock, unloadWhy.c_str(), _TRUNCATE);
				strncpy_s(out[i].reloadBlock, reloadWhy.c_str(), _TRUNCATE);

				PluginGuard::Fault fault;
				out[i].isCrashed      = p.hModule && PluginGuard::GetFault(p.hModule, &fault);
				out[i].isDenied       = p.hModule && PluginGuard::IsDenied(p.hModule);
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
		const int index = IndexByName(pluginName);
		// Nothing to notify while its PluginShutdown runs.
		if (index >= 0 && !g_loadedPlugins[index]->isUnloading)
			result = &g_loadedPlugins[index]->self;
		LeaveCriticalSection(&g_pluginLock);
		return result;
	}

	WireCompat::Decl GetWireDecl(const char* pluginName)
	{
		WireCompat::Decl result;
		if (!pluginName) return result;

		EnterCriticalSection(&g_pluginLock);
		const int index = IndexByName(pluginName);
		if (index >= 0)
			result = g_loadedPlugins[index]->wire;
		LeaveCriticalSection(&g_pluginLock);
		return result;
	}

	std::vector<WireCompat::Entry> GetWireEntries()
	{
		std::vector<WireCompat::Entry> result;
		EnterCriticalSection(&g_pluginLock);
		for (const auto& p : g_loadedPlugins)
		{
			if (!p->isInitialized) continue;
			WireCompat::Entry e;
			e.name = p->cachedName;
			e.version = p->cachedVersion;
			e.decl = p->wire;
			result.push_back(std::move(e));
		}
		LeaveCriticalSection(&g_pluginLock);
		return result;
	}

	int GetInterfaceVersionForModule(HMODULE module)
	{
		if (!module) return 0;

		EnterCriticalSection(&g_pluginLock);
		int result = 0;
		for (auto& p : g_loadedPlugins)
		{
			if (p->hModule == module)
			{
				result = p->interfaceVersion;
				break;
			}
		}
		LeaveCriticalSection(&g_pluginLock);
		return result;
	}

	// Everything after PluginShutdown: what the plugin registered that lives in
	// its module, then FreeLibrary. Caller holds g_pluginLock. False when one of
	// its renders is still running: it is shut down, but stays mapped.
	static bool FreeShutDownPlugin(LoadedPlugin& p, const wchar_t* action)
	{
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
		p.wire    = WireCompat::Decl{};
		++g_pluginGeneration;
		return true;
	}

	// PluginShutdown, then FreeShutDownPlugin. Caller holds g_pluginLock the
	// whole time: for a plugin that was only just initialized, inside
	// InitPluginRecord.
	static bool ShutDownAndFree(LoadedPlugin& p, const wchar_t* action)
	{
		if (!CallShutdown(p.shutdown))
			LogPluginCrash(p.cachedName.c_str(), p.hModule, L"PluginShutdown (unload)");
		p.isInitialized = false;
		ReloadPolicy::SetBusyReason(&p.self, nullptr);
		return FreeShutDownPlugin(p, action);
	}

	// PluginShutdown of a running plugin, called with g_pluginLock released.
	// Caller holds g_pluginLock once, and holds it again on return.
	//
	// A plugin's PluginShutdown may wait for work that only the render thread
	// can do (a GPU readback and its fence, say), and the render thread takes
	// g_pluginLock every frame it draws the plugin list or the overlay. Run
	// under the lock, the two wait for each other until the plugin gives up.
	//
	// While the lock is released the record stays in g_loadedPlugins with
	// isUnloading set, and nothing in it changes: isInitialized is already
	// false, so GetLoadedPluginCount and GetLoadedPluginInfos no longer list the
	// plugin, a second UnloadPlugin or ReloadPlugin for it is refused, and
	// UnloadAllPlugins waits for it. No other path writes the record or calls
	// into the plugin until the lock is taken again, so FreeShutDownPlugin, run
	// right after, still has the record to itself. The generation moves here, as
	// the plugin leaves the list, so the Config tab does not keep showing the
	// page of whichever plugin now sits at its index.
	static void ShutDownWithoutLock(LoadedPlugin& p, const wchar_t* context)
	{
		p.isInitialized = false;
		p.isUnloading   = true;
		++g_pluginGeneration;   // it drops out of the plugin list here
		LeaveCriticalSection(&g_pluginLock);

		if (!CallShutdown(p.shutdown))
			LogPluginCrash(p.cachedName.c_str(), p.hModule, context);

		EnterCriticalSection(&g_pluginLock);
		p.isUnloading = false;
		ReloadPolicy::SetBusyReason(&p.self, nullptr);
	}

	bool UnloadPlugin(int index, std::string* refusal)
	{
		if (refusal) refusal->clear();
		EnterCriticalSection(&g_pluginLock);

		if (index >= 0 && index < static_cast<int>(g_loadedPlugins.size()) &&
			g_loadedPlugins[index]->isUnloading)
		{
			ModLoaderLogger::LogWarn(L"Unload of %S refused: %S", g_loadedPlugins[index]->cachedName.c_str(), kUnloadingNow);
			LeaveCriticalSection(&g_pluginLock);
			if (refusal) *refusal = kUnloadingNow;
			return false;
		}

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
		ShutDownWithoutLock(p, L"PluginShutdown (unload)");
		const bool freed = FreeShutDownPlugin(p, L"Unload");
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

#ifdef MODLOADER_CLIENT_BUILD
		// Asked before anything is shut down: a plugin that may not load again
		// keeps running.
		if (IsListedDisabled(p.fileName))
		{
			p.isDisabled = true;
			ModLoaderLogger::LogWarn(L"Load of %s refused: %S", BaseFileName(p.fileName), kDisabledWhy);
			LeaveCriticalSection(&g_pluginLock);
			if (refusal) *refusal = kDisabledWhy;
			return false;
		}
		const char* blocked = nullptr;
		if (IsStale(p.fileName))
			blocked = kStaleWhy;
		else if (GetFileAttributesW(p.fileName.c_str()) == INVALID_FILE_ATTRIBUTES)
			blocked = "its file is not in ModLoader\\Plugins";
		if (blocked)
		{
			ModLoaderLogger::LogWarn(L"Load of %s refused: %S", BaseFileName(p.fileName), blocked);
			LeaveCriticalSection(&g_pluginLock);
			if (refusal) *refusal = blocked;
			return false;
		}
		p.isUninstalled = false; // a file is back at its path
#endif

		if (p.isUnloading)
		{
			ModLoaderLogger::LogWarn(L"Reload of %S refused: %S", p.cachedName.c_str(), kUnloadingNow);
			LeaveCriticalSection(&g_pluginLock);
			if (refusal) *refusal = kUnloadingNow;
			return false;
		}

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
			ShutDownWithoutLock(p, L"PluginShutdown (reload)");
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
				WriteDeclaredFeaturesDump();
				return false;
			}

			PluginGuard::Untrack(p.hModule);
			FreeLibrary(p.hModule);
			p.hModule = nullptr;
			p.info    = nullptr;
			p.wire    = WireCompat::Decl{};
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

		WriteDeclaredFeaturesDump();
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

	// ---- Developer remote reload (plugins/remote_reload.h)

	// The name and version of the record at index, cached at load so they survive FreeLibrary.
	static void RecordNameAndVersion(int index, std::string& name, std::string& version)
	{
		EnterCriticalSection(&g_pluginLock);
		if (index >= 0 && index < static_cast<int>(g_loadedPlugins.size()))
		{
			name    = g_loadedPlugins[index]->cachedName;
			version = g_loadedPlugins[index]->cachedVersion;
		}
		LeaveCriticalSection(&g_pluginLock);
	}

	static void RemoteReloadOne(const std::string& asked)
	{
		const int index = FindPluginIndex(asked.c_str());
		if (index < 0)
		{
			ModLoaderLogger::LogWarn(L"%S", RemoteReload::NoSuchPlugin(asked).c_str());
			return;
		}

		std::string why;
		if (ReloadPlugin(index, &why) && IsRecordInitialized(index))
		{
			std::string name, version;
			RecordNameAndVersion(index, name, version);
			ModLoaderLogger::LogMessage(L"%S", RemoteReload::Reloaded(name.empty() ? asked : name, version).c_str());
		}
		else
		{
			ModLoaderLogger::LogWarn(L"%S", RemoteReload::Failed(asked,
				why.empty() ? "it did not load or initialize again (see above)" : why).c_str());
		}
	}

	// Reads and removes the request file. Renaming it away first is what claims
	// it: if the writer still has it open the rename fails and the next poll
	// tries again, and once the rename worked nothing can make a second poll
	// read the same request, whatever happens to the claimed copy.
	static bool TakeRemoteReloadRequest(const std::wstring& dir, std::string& text)
	{
		const std::wstring request = dir + RemoteReload::kRequestFile;
		const std::wstring claimed = dir + RemoteReload::kClaimedFile;

		if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
			return false;
		if (!MoveFileExW(request.c_str(), claimed.c_str(), MOVEFILE_REPLACE_EXISTING))
			return false;

		HANDLE file = CreateFileW(claimed.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			text.assign(RemoteReload::kMaxBytes, '\0');
			DWORD got = 0;
			if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr))
				got = 0;
			text.resize(got);
			CloseHandle(file);
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[RemoteReload] Could not open the request file, so it was dropped (error %lu)", GetLastError());
		}
		if (!DeleteFileW(claimed.c_str()))
			ModLoaderLogger::LogWarn(L"[RemoteReload] Could not remove the handled request file (error %lu)", GetLastError());
		return file != INVALID_HANDLE_VALUE;
	}

	void PumpRemoteReload()
	{
		// The ini is read once. Off is one flag test per tick.
		static const bool enabled = []()
		{
			const bool on = RemoteReload::IsEnabled(GetModLoaderDirPath(L"modloader.ini"));
			if (on)
				ModLoaderLogger::LogWarn(L"[RemoteReload] modloader.ini [Developer] RemoteReload=1: a file named %s in the ModLoader folder reloads plugins",
					RemoteReload::kRequestFile);
			return on;
		}();
		if (!enabled || !g_managerInitialized || CrashRestart::Global().Stopped())
			return;

		static RemoteReload::Throttle throttle;
		if (!throttle.Due(GetTickCount64()))
			return;

		static const std::wstring dir = GetModLoaderDirPath(L"");
		std::string text;
		if (!TakeRemoteReloadRequest(dir, text))
			return;

		const RemoteReload::Request req = RemoteReload::Parse(text);
		for (const std::string& name : req.names)
			RemoteReloadOne(name);
		if (req.refused)
			ModLoaderLogger::LogWarn(L"%S", RemoteReload::TooMany(req.refused).c_str());
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

		// PluginInfo name first -- that is what `plugins` displays and what a
		// user is most likely to type.
		int found = IndexByName(nameOrFile);

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
		if (index >= 0 && !g_loadedPlugins[index]->isUnloading)
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
		case ScanStatus::Disabled:     return "Disabled";
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
			{
				char need[160];
				DescribeLoaderNeed(why.interfaceVersion, need, sizeof(need));
				snprintf(text, sizeof(text),
					"Built for plugin interface v%d, newer than this loader (%s accepts up to v%d). %s.",
					why.interfaceVersion, MODLOADER_BUILD_TAG, PLUGIN_INTERFACE_VERSION_MAX, need);
			}
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
		case LoadFailure::Disabled:
			r.status = ScanStatus::Disabled;
			snprintf(text, sizeof(text), "It is %s.", kDisabledWhy);
			break;
		case LoadFailure::NeedsRestart:
			r.status = ScanStatus::NeedsRestart;
			snprintf(text, sizeof(text), "Not loaded: %s.", kStaleWhy);
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

	// ScanForNewPlugins, or with `only` the same for that one path.
	static int ScanPlugins(std::vector<ScanResult>* outResults, const std::wstring* only)
	{
		if (!g_managerInitialized)
		{
			ModLoaderLogger::LogMessage(L"ERROR: Plugin manager not initialized");
			return 0;
		}

		std::vector<std::wstring> dllPaths;
		if (only)
			dllPaths.push_back(*only);
		else if (!CollectPluginDllPaths(dllPaths))
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
		for (const std::string& file : only ? std::vector<std::string>() : PreloadManager::FindNewDlls())
		{
			ModLoaderLogger::LogMessage(L"ScanForNewPlugins: new Preload DLL %S needs a game restart", file.c_str());
			if (outResults)
				outResults->push_back({ ScanStatus::NeedsRestart, file, file,
					"A Preload plugin runs before the game starts. Restart the game to load it." });
		}

		ModLoaderLogger::LogMessage(L"ScanForNewPlugins: %d new DLL(s) loaded, %d initialized", loadedCount, initedCount);
		return initedCount;
	}

	int ScanForNewPlugins(std::vector<ScanResult>* outResults)
	{
#ifdef MODLOADER_CLIENT_BUILD
		// First, so that the scan below does not list one twice.
		const int returned = g_managerInitialized ? LoadReturnedCopies(outResults) : 0;
		const int initedCount = returned + ScanPlugins(outResults, nullptr);
		if (g_managerInitialized)
			RefreshManagedFiles();
#else
		const int initedCount = ScanPlugins(outResults, nullptr);
#endif
		return initedCount;
	}

#ifdef MODLOADER_CLIENT_BUILD
	// ---- Disable, enable, uninstall, restore and install -------------------
	// Everything below that is named s_* is guarded by g_pluginLock. The lock
	// is not held across file reads, nor when calling UnloadPlugin or
	// ReloadPlugin: ShutDownWithoutLock must be able to release it.

	struct ManageState
	{
		bool working = false;
		bool enableWaitsForRestart = false;
		ManageOp lastOp = ManageOp::Disable;
		ManageResult lastResult = ManageResult::None;
		std::string message;
		// Install only: it has a card before it has a file.
		std::string fileName, name;
		bool listSigned = false;
	};

	static constexpr const char* kAnotherInstall = "Another mod is installing. Try again when it finishes.";
	static bool s_installing = false;
	static std::map<std::wstring, ManageState> s_manageStates; // by LowerCase(file name)
	static std::vector<std::pair<std::wstring, ManagedPlugin>> s_managedFiles; // path, what was read
	static std::vector<std::string> s_preloadNames;

	static std::string Narrow(const std::wstring& text, UINT codePage = CP_ACP)
	{
		const int length = WideCharToMultiByte(codePage, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (length <= 1) return {};
		std::string out(length - 1, '\0');
		WideCharToMultiByte(codePage, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
		return out;
	}

	static std::wstring Widen(const std::string& text, UINT codePage = CP_ACP)
	{
		const int length = MultiByteToWideChar(codePage, 0, text.c_str(), -1, nullptr, 0);
		if (length <= 1) return {};
		std::wstring out(length - 1, L'\0');
		MultiByteToWideChar(codePage, 0, text.c_str(), -1, out.data(), length);
		return out;
	}

	// "...\Win64\ModLoader\" + name
	static PluginManage::Folders ManageFolders()
	{
		return { GetModLoaderDirPath(L"Plugins"), GetModLoaderDirPath(L"Removed") };
	}

	// The game exe's folder, no trailing backslash.
	static std::wstring GameDir()
	{
		std::wstring dir = GetExeDir();
		if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
		return dir;
	}

	// The record for a DLL file name, or -1. Caller holds g_pluginLock.
	static int IndexByFile(const std::wstring& fileName)
	{
		for (int i = 0; i < static_cast<int>(g_loadedPlugins.size()); ++i)
			if (PluginManage::SameName(BaseFileName(g_loadedPlugins[i]->fileName), fileName))
				return i;
		return -1;
	}

	static void SaveIdentityOf(const LoadedPlugin& rec)
	{
		// PluginInfo strings are UTF-8 or plain ASCII.
		PluginManage::SaveIdentity(GetModLoaderDirPath(PluginManage::kIdentityFile), BaseFileName(rec.fileName),
			{ Widen(rec.cachedName, CP_UTF8), Widen(rec.cachedVersion, CP_UTF8), Widen(rec.cachedAuthor, CP_UTF8) });
	}

	// A disabled record loads nothing, so its strings come from the cache.
	static void FillCachedIdentity(LoadedPlugin& rec)
	{
		if (!rec.cachedName.empty()) return;
		const PluginManage::Identity who = PluginManage::ReadIdentity(
			GetModLoaderDirPath(PluginManage::kIdentityFile), BaseFileName(rec.fileName));
		rec.cachedName    = Narrow(who.name, CP_UTF8);
		rec.cachedVersion = Narrow(who.version, CP_UTF8);
		rec.cachedAuthor  = Narrow(who.author, CP_UTF8);
	}

	void RefreshManagedFiles()
	{
		const PluginManage::Folders folders = ManageFolders();
		std::vector<std::wstring> paths;
		CollectPluginDllPaths(paths);

		std::vector<std::wstring> disabled;
		PluginManage::ReadDisabledList(GetModLoaderDirPath(L"modloader.ini"), disabled);
		const std::vector<PluginManage::Backup> backups = PluginManage::ListBackups(folders);

		const std::wstring identityIni = GetModLoaderDirPath(PluginManage::kIdentityFile);

		const VortexNotice::Deployment vortex = VortexNotice::ReadDeployment(GameDir());

		std::vector<std::pair<std::wstring, ManagedPlugin>> files;
		const auto entry = [&](const std::wstring& path) -> ManagedPlugin&
		{
			for (auto& f : files)
				if (PluginManage::SameName(BaseFileName(f.first), BaseFileName(path)))
					return f.second;
			ManagedPlugin m;
			const std::wstring file = BaseFileName(path);
			m.fileName = Narrow(file);
			m.facts.canBeDisabled = PluginManage::CanBeDisabled(file);
			m.facts.listedDisabled = PluginManage::ListHas(disabled, file);
			const PluginManage::Identity who = PluginManage::ReadIdentity(identityIni, file);
			m.name = Narrow(who.name, CP_UTF8);
			m.version = Narrow(who.version, CP_UTF8);
			m.author = Narrow(who.author, CP_UTF8);
			files.emplace_back(path, std::move(m));
			return files.back().second;
		};

		for (const std::wstring& path : paths)
		{
			if (!PluginManage::IsPluginDllName(BaseFileName(path))) continue;
			ManagedPlugin& m = entry(path);
			m.facts.inPlugins = true;

			const std::string stem = "ModLoader\\Plugins\\" + Narrow(PluginManage::Stem(BaseFileName(path)), CP_UTF8);
			const VortexNotice::Listing listing = vortex.Lists(
				{ stem + ".dll", stem + ".pdb", stem + ".json" });
			m.facts.vortexListed = listing == VortexNotice::Listing::Listed;
			m.vortexUnsure = listing == VortexNotice::Listing::Unsure;
		}
		for (const PluginManage::Backup& backup : backups)
		{
			ManagedPlugin& m = entry(folders.plugins + L"\\" + backup.dllName);
			m.facts.hasBackup = true;
			if (m.name.empty())
			{
				m.name = Narrow(backup.name, CP_UTF8);
				m.version = Narrow(backup.version, CP_UTF8);
			}
		}

		std::vector<std::string> preloadNames;
		for (const PreloadManager::Record& r : PreloadManager::GetRecords())
			PluginManage::AddPreloadNames(r.fileName, r.name == r.fileName ? std::string() : r.name, preloadNames);
		for (const std::string& file : PreloadManager::FindNewDlls())
			PluginManage::AddPreloadNames(file, std::string(), preloadNames);

		EnterCriticalSection(&g_pluginLock);
		// A file that is gone but whose code is still mapped keeps its card.
		for (const auto& rec : g_loadedPlugins)
			if (rec->hModule)
				entry(rec->fileName);
		for (const auto& rec : g_loadedPlugins)
			if (!rec->isUnloading) // nothing writes a record while it unloads
				rec->isDisabled = PluginManage::ListHas(disabled, BaseFileName(rec->fileName));
		// A DLL that is not a plugin gets no card.
		files.erase(std::remove_if(files.begin(), files.end(), [](const auto& f)
			{
				return f.second.facts.inPlugins && !f.second.facts.hasBackup && s_notPlugins.count(LowerCase(f.first));
			}), files.end());
		s_managedFiles = std::move(files);
		s_preloadNames = std::move(preloadNames);
		LeaveCriticalSection(&g_pluginLock);
	}

	ManageSnapshot GetManageSnapshot()
	{
		ManageSnapshot out;
		if (!g_managerInitialized) return out;

		const auto apply = [](ManagedPlugin& m, const ManageState& state)
		{
			m.facts.working = state.working;
			m.facts.enableWaitsForRestart = state.enableWaitsForRestart;
			m.lastOp = state.lastOp;
			m.lastResult = state.lastResult;
			m.message = state.message;
			m.listSigned = state.lastOp == ManageOp::Install && state.listSigned;
		};

		EnterCriticalSection(&g_pluginLock);
		out.preloadNames = s_preloadNames;
		out.installing = s_installing;
		std::set<std::wstring> carded;
		for (const auto& [path, read] : s_managedFiles)
		{
			ManagedPlugin m = read;
			const std::wstring file = BaseFileName(path);
			const int index = IndexByFile(file);
			if (index >= 0)
			{
				const LoadedPlugin& rec = *g_loadedPlugins[index];
				m.facts.running = rec.hModule != nullptr;
				if (!rec.cachedName.empty())
				{
					m.name = rec.cachedName;
					m.version = rec.cachedVersion;
					m.author = rec.cachedAuthor;
				}
			}
			const auto state = s_manageStates.find(LowerCase(file));
			if (state != s_manageStates.end())
				apply(m, state->second);
			carded.insert(LowerCase(file));
			out.plugins.push_back(std::move(m));
		}
		// An install that has written nothing, so far or at all.
		for (const auto& [key, state] : s_manageStates)
		{
			if (state.lastOp != ManageOp::Install || carded.count(key))
				continue;
			ManagedPlugin m;
			m.fileName = state.fileName;
			m.name = state.name;
			apply(m, state);
			out.plugins.push_back(std::move(m));
		}
		LeaveCriticalSection(&g_pluginLock);
		return out;
	}

	struct ChangeOutcome
	{
		ManageResult result = ManageResult::Done;
		std::string message;
		bool enableWaitsForRestart = false;
	};

	static bool IsStillMapped(const std::wstring& fileName)
	{
		EnterCriticalSection(&g_pluginLock);
		const int index = IndexByFile(fileName);
		const bool mapped = index >= 0 && g_loadedPlugins[index]->hModule;
		LeaveCriticalSection(&g_pluginLock);
		return mapped;
	}

	// Load an installed, enabled plugin now, or say that it waits for a restart.
	static ChangeOutcome LoadNow(const std::wstring& fileName)
	{
		const std::wstring path = ManageFolders().plugins + L"\\" + fileName;
		const ChangeOutcome later{ ManageResult::AfterRestart, "It will be on after you restart the game.", true };
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			return { ManageResult::Failed, "Its file is not in ModLoader\\Plugins." };

		EnterCriticalSection(&g_pluginLock);
		const int index = IndexByFile(fileName);
		bool running = false, wait = IsStale(path);
		if (index >= 0)
		{
			LoadedPlugin& rec = *g_loadedPlugins[index];
			rec.isUninstalled = false;
			running = rec.hModule && rec.isInitialized;
			// ReloadPlugin does not look for a second plugin under the name.
			wait = wait || (rec.hModule && !running) || (!running && OtherPluginNamed(rec, true));
		}
		LeaveCriticalSection(&g_pluginLock);

		if (running) return {};
		if (wait) return later;

		if (index >= 0)
		{
			std::string why;
			if (ReloadPlugin(index, &why)) return {};
			return why.empty() ? ChangeOutcome{ ManageResult::Failed, "It could not be loaded. See ModLoader.log." }
			                   : ChangeOutcome{ ManageResult::AfterRestart, why, true };
		}

		std::vector<ScanResult> results;
		ScanPlugins(&results, &path);
		if (results.empty() || results.front().status == ScanStatus::Live || results.front().status == ScanStatus::Late)
			return {};
		if (results.front().status == ScanStatus::Refused || results.front().status == ScanStatus::Duplicate)
			return { ManageResult::AfterRestart, results.front().action, true };
		return { ManageResult::Failed, results.front().action };
	}

	// A record stays after an uninstall, so a scan takes its path as known. A
	// DLL put back there since is loaded here.
	static int LoadReturnedCopies(std::vector<ScanResult>* outResults)
	{
		std::vector<std::wstring> paths;
		EnterCriticalSection(&g_pluginLock);
		for (const auto& rec : g_loadedPlugins)
			if (rec->isUninstalled && !rec->isUnloading)
				paths.push_back(rec->fileName);
		LeaveCriticalSection(&g_pluginLock);

		int liveCount = 0;
		for (const std::wstring& path : paths)
		{
			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
				continue;
			const std::wstring file = BaseFileName(path);
			ScanResult r;
			r.fileName = Narrow(file);
			// The old code is still mapped, whether or not it is still running.
			if (IsStale(path) || IsStillMapped(file))
			{
				r.status = ScanStatus::NeedsRestart;
				r.action = std::string("Not loaded: ") + kStaleWhy + ".";
			}
			else
			{
				const ChangeOutcome outcome = LoadNow(file);
				r.status = outcome.result == ManageResult::Done ? ScanStatus::Live
					: outcome.result == ManageResult::AfterRestart ? ScanStatus::NeedsRestart
					: ScanStatus::Failed;
				r.action = outcome.message;
				if (r.status == ScanStatus::Live)
					++liveCount;
			}
			EnterCriticalSection(&g_pluginLock);
			const int index = IndexByFile(file);
			r.name = index >= 0 ? g_loadedPlugins[index]->cachedName : r.fileName;
			LeaveCriticalSection(&g_pluginLock);
			if (outResults)
				outResults->push_back(std::move(r));
		}
		return liveCount;
	}

	// The other DLL in Plugins that is already the plugin called `name`, or
	// empty. A restore beside it would leave two copies to load at the next start.
	static std::wstring OtherCopyOf(const std::wstring& name, const std::wstring& fileName)
	{
		std::vector<std::wstring> paths;
		if (name.empty() || !CollectPluginDllPaths(paths))
			return {};

		std::vector<std::wstring> disabled;
		PluginManage::ReadDisabledList(GetModLoaderDirPath(L"modloader.ini"), disabled);
		const std::wstring identityIni = GetModLoaderDirPath(PluginManage::kIdentityFile);
		for (const std::wstring& path : paths)
		{
			const std::wstring file = BaseFileName(path);
			if (PluginManage::SameName(file, fileName))
				continue;

			std::wstring other;
			EnterCriticalSection(&g_pluginLock);
			const int index = IndexByFile(file);
			if (index >= 0 && !g_loadedPlugins[index]->isUninstalled)
				other = Widen(g_loadedPlugins[index]->cachedName, CP_UTF8);
			LeaveCriticalSection(&g_pluginLock);
			if (other.empty())
				other = PluginManage::ReadIdentity(identityIni, file).name;
			// A DLL put there since the last start. None of a disabled one is read.
			if (other.empty() && !PluginManage::ListHas(disabled, file))
				other = Widen(PluginDllVersion::Read(path.c_str(), &PluginInfo::name), CP_UTF8);

			if (PluginManage::SameName(other, name))
				return file;
		}
		return {};
	}

	static ChangeOutcome ApplyChange(ManageOp op, const std::wstring& fileName)
	{
		if (!g_startupComplete)
			return { ManageResult::NotReady, "The plugins are still starting." };

		const std::wstring ini = GetModLoaderDirPath(L"modloader.ini");
		const PluginManage::Folders folders = ManageFolders();

		EnterCriticalSection(&g_pluginLock);
		const int index = IndexByFile(fileName);
		LoadedPlugin* rec = index >= 0 ? g_loadedPlugins[index].get() : nullptr;
		if (rec && rec->isUnloading)
		{
			LeaveCriticalSection(&g_pluginLock);
			return { ManageResult::Failed, kUnloadingNow };
		}
		const std::string name = rec ? rec->cachedName : std::string();
		const std::string version = rec ? rec->cachedVersion : std::string();
		const bool initialized = rec && rec->isInitialized;
		LeaveCriticalSection(&g_pluginLock);

		switch (op)
		{
		case ManageOp::Disable:
		{
			// Saved first: if the plugin cannot stop now it is off next start.
			if (PluginManage::SaveDisabled(ini, fileName, true) != PluginManage::SaveResult::Ok)
				return { ManageResult::SaveFailed, "Couldn't save the change. Nothing was changed." };
			if (rec)
			{
				EnterCriticalSection(&g_pluginLock);
				rec->isDisabled = true;
				SaveIdentityOf(*rec);
				LeaveCriticalSection(&g_pluginLock);
			}
			std::string why;
			if (initialized) UnloadPlugin(index, &why);
			if (IsStillMapped(fileName))
				return { ManageResult::AfterRestart, why };
			return {};
		}
		case ManageOp::Enable:
			if (PluginManage::SaveDisabled(ini, fileName, false) != PluginManage::SaveResult::Ok)
				return { ManageResult::SaveFailed, "Couldn't save the change. Nothing was changed." };
			if (rec)
			{
				EnterCriticalSection(&g_pluginLock);
				rec->isDisabled = false;
				LeaveCriticalSection(&g_pluginLock);
			}
			return LoadNow(fileName);

		case ManageOp::Uninstall:
		{
			const std::string stem = "ModLoader\\Plugins\\" + Narrow(PluginManage::Stem(fileName), CP_UTF8);
			if (VortexNotice::ReadDeployment(GameDir()).Lists({ stem + ".dll", stem + ".pdb", stem + ".json" })
				== VortexNotice::Listing::Listed)
				return { ManageResult::Vortex, "Vortex installed this mod. Uninstall it in Vortex." };

			PluginManage::Identity who{ Widen(name, CP_UTF8), Widen(version, CP_UTF8), {} };
			if (who.name.empty())
				who = PluginManage::ReadIdentity(GetModLoaderDirPath(PluginManage::kIdentityFile), fileName);

			// The files move first, so a failed move leaves the plugin as it was.
			std::vector<std::wstring> disabled;
			PluginManage::ReadDisabledList(ini, disabled);
			const std::wstring path = folders.plugins + L"\\" + fileName;
			const bool alreadyStale = IsStale(path);
			SYSTEMTIME now{};
			GetSystemTime(&now);
			const PluginManage::Outcome moved = PluginManage::Uninstall(folders, fileName, who.name, who.version,
				PluginManage::BackupStamp(now), PluginManage::MoveOnSameVolume,
				PluginManage::ListHas(disabled, fileName));
			if (moved.result != PluginManage::FileResult::Ok)
			{
				ModLoaderLogger::LogWarn(L"Uninstall of %s failed on %s (result %d, error %lu)", fileName.c_str(),
					moved.failedFile.c_str(), static_cast<int>(moved.result), moved.error);
				if (moved.result == PluginManage::FileResult::RollbackFailed)
				{
					// The DLL may be the file that stayed moved.
					if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
					{
						EnterCriticalSection(&g_pluginLock);
						s_uninstalledPaths.emplace(LowerCase(path), moved.backupDir);
						LeaveCriticalSection(&g_pluginLock);
					}
					return { ManageResult::RollbackFailed,
						"Uninstall only partly worked. Check ModLoader\\Plugins and ModLoader\\Removed." };
				}
				return { ManageResult::MoveFailed, "Couldn't uninstall it. Nothing was changed." };
			}
			// A copy installed later starts enabled. The backup remembers the rest,
			// and a restore sets the list again whether or not this write works.
			PluginManage::SaveDisabled(ini, fileName, false);

			EnterCriticalSection(&g_pluginLock);
			// A module still mapped under this path must not be handed back
			// for a new file put there. An older one keeps its entry.
			if (!alreadyStale)
				s_uninstalledPaths[LowerCase(path)] = moved.backupDir;
			if (rec)
			{
				SaveIdentityOf(*rec);
				rec->isUninstalled = true;
				rec->isDisabled = false;
				++g_pluginGeneration;
			}
			LeaveCriticalSection(&g_pluginLock);

			std::string why;
			if (initialized) UnloadPlugin(index, &why);
			if (IsStillMapped(fileName))
				return { ManageResult::AfterRestart, why };
			return {};
		}
		case ManageOp::Restore:
		{
			PluginManage::Backup backup;
			const bool found = PluginManage::FindNewestBackup(folders, fileName, backup) == PluginManage::FileResult::Ok;
			if (const std::wstring other = found ? OtherCopyOf(backup.name, fileName) : std::wstring(); !other.empty())
				return { ManageResult::AlreadyInstalled, "A copy is already installed as " + Narrow(other, CP_UTF8) +
					", so the old one stays in ModLoader\\Removed." };

			// The plugin comes back as it was. Set first: a plugin that was
			// disabled must never be in Plugins without its entry.
			std::vector<std::wstring> disabled;
			PluginManage::ReadDisabledList(ini, disabled);
			const bool wasListed = PluginManage::ListHas(disabled, fileName);
			const bool relist = found && wasListed != backup.disabled;
			if (relist && PluginManage::SaveDisabled(ini, fileName, backup.disabled) != PluginManage::SaveResult::Ok)
				return { ManageResult::SaveFailed, "Couldn't save the change. Nothing was changed." };

			const PluginManage::Outcome moved = PluginManage::Restore(folders, fileName);
			if (relist && moved.result != PluginManage::FileResult::Ok &&
			    moved.result != PluginManage::FileResult::RollbackFailed)
				PluginManage::SaveDisabled(ini, fileName, wasListed);
			if (moved.result == PluginManage::FileResult::AlreadyInstalled)
				return { ManageResult::AlreadyInstalled,
					"A copy is already installed, so the old one stays in ModLoader\\Removed." };
			if (moved.result == PluginManage::FileResult::RollbackFailed)
				return { ManageResult::RollbackFailed,
					"Restore only partly worked. Check ModLoader\\Plugins and ModLoader\\Removed." };
			if (moved.result != PluginManage::FileResult::Ok)
				return { ManageResult::MoveFailed, "Couldn't restore it. Nothing was changed." };

			const std::wstring path = folders.plugins + L"\\" + fileName;
			EnterCriticalSection(&g_pluginLock);
			// The file whose code may still be mapped is back: no longer stale.
			const auto stale = s_uninstalledPaths.find(LowerCase(path));
			if (stale != s_uninstalledPaths.end() && stale->second == moved.backupDir)
				s_uninstalledPaths.erase(stale);
			const int restored = IndexByFile(fileName);
			if (restored >= 0)
			{
				g_loadedPlugins[restored]->isUninstalled = false;
				g_loadedPlugins[restored]->isDisabled = backup.disabled;
				++g_pluginGeneration;
			}
			LeaveCriticalSection(&g_pluginLock);

			if (!backup.disabled)
				return LoadNow(fileName);
			// Lists it as disabled. None of its code is loaded.
			if (restored < 0)
				ScanPlugins(nullptr, &path);
			return {};
		}
		}
		return { ManageResult::Failed };
	}

	static void RecordOutcome(const std::wstring& file, ManageOp op, const ChangeOutcome& outcome)
	{
		EnterCriticalSection(&g_pluginLock);
		ManageState& done = s_manageStates[LowerCase(file)];
		done.working = false;
		done.lastOp = op;
		done.lastResult = outcome.result;
		done.message = outcome.message;
		done.enableWaitsForRestart = outcome.enableWaitsForRestart;
		LeaveCriticalSection(&g_pluginLock);
	}

	void RequestPluginChange(ManageOp op, const std::string& fileName)
	{
		const std::wstring file = Widen(fileName);
		if (!g_managerInitialized || op == ManageOp::Install || !PluginManage::IsPluginDllName(file)) return;

		EnterCriticalSection(&g_pluginLock);
		ManageState& state = s_manageStates[LowerCase(file)];
		const bool busy = state.working;
		if (!busy)
		{
			state.working = true;
			state.lastOp = op;
			state.lastResult = ManageResult::None;
			state.message.clear();
		}
		LeaveCriticalSection(&g_pluginLock);
		if (busy) return;

		GameThreadDispatch::PostVoid([op, file]
		{
			const ChangeOutcome outcome = ApplyChange(op, file);
			RefreshManagedFiles();
			RecordOutcome(file, op, outcome);
		});
	}

	static ModInstall::Need NeedState(const ModIndex::Entry& needed, const ManageSnapshot& managed)
	{
		bool disabled = false;
		for (const ManagedPlugin& plugin : managed.plugins)
		{
			if (!plugin.facts.inPlugins || !ListingMatches(needed, plugin))
				continue;
			if (!plugin.facts.listedDisabled)
				return ModInstall::Need::Met;
			disabled = true;
		}
		if (ModIndex::IsInstalled(needed, managed.preloadNames))
			return ModInstall::Need::Met;
		return disabled ? ModInstall::Need::Disabled : ModInstall::Need::Missing;
	}

	std::string InstallBlockedReason(const ModIndex::Entry& entry, const std::vector<ModIndex::Entry>& list,
		const ManageSnapshot& managed)
	{
		// A server-only plugin is never installed on a PC, whatever install fields it has.
		if (entry.serverOnly || !ModIndex::detail::IsInstallDllName(entry.install.dllFile))
			return "This mod can't be installed from here. Open its page.";
		if (entry.interfaceVersionMin > PLUGIN_INTERFACE_VERSION_MAX)
			return "Needs a newer ModLoader.";
		for (const ManagedPlugin& plugin : managed.plugins)
			if (plugin.facts.vortexListed && ListingMatches(entry, plugin))
				return ModInstall::kVortexManaged;
		if (managed.installing)
			return kAnotherInstall;

		std::vector<ModInstall::Requirement> needs;
		for (const std::string& id : entry.requiredIds)
		{
			ModIndex::Entry needed;
			needed.id = needed.name = id;
			for (const ModIndex::Entry& listed : list)
				if (ModIndex::detail::EqualsNoCase(listed.id, id))
				{
					needed = listed;
					break;
				}
			needs.push_back({ needed.name, NeedState(needed, managed) });
		}
		return ModInstall::RequirementMessage(entry.name, needs);
	}

	// Game thread. The new files are in Plugins: clear an old Disabled entry,
	// then load them.
	static ChangeOutcome FinishInstall(const std::wstring& fileName, const std::string& version)
	{
		// Before the load, which only ever raises the record.
		SetInstalledPluginVersion(fileName.c_str(), version.c_str());
		if (PluginManage::SaveDisabled(GetModLoaderDirPath(L"modloader.ini"), fileName, false) !=
		    PluginManage::SaveResult::Ok)
			return { ManageResult::SaveFailed, "Installed, but still disabled." };

		EnterCriticalSection(&g_pluginLock);
		// What was learned about an earlier file at this path.
		s_notPlugins.erase(LowerCase(ManageFolders().plugins + L"\\" + fileName));
		const int index = IndexByFile(fileName);
		LoadedPlugin* rec = index >= 0 ? g_loadedPlugins[index].get() : nullptr;
		const bool unloading = rec && rec->isUnloading;
		// The files are new, so code mapped under this name is an older copy.
		const bool oldCodeMapped = rec && rec->hModule;
		if (rec && !unloading)
			rec->isDisabled = false;
		LeaveCriticalSection(&g_pluginLock);

		const ChangeOutcome later{ ManageResult::AfterRestart, "It will be on after you restart the game.", true };
		if (!g_startupComplete || unloading)
			return later;
		const ChangeOutcome loaded = LoadNow(fileName);
		return oldCodeMapped ? later : loaded;
	}

	static void LogInstall(const std::string& name, const ChangeOutcome& outcome)
	{
		if (outcome.result == ManageResult::Done)
			ModLoaderLogger::LogInfo(L"[Install] %hs: installed and loaded", name.c_str());
		else if (outcome.result == ManageResult::AfterRestart)
			ModLoaderLogger::LogInfo(L"[Install] %hs: installed, waits for a restart (%hs)", name.c_str(),
				outcome.message.c_str());
		else
			ModLoaderLogger::LogWarn(L"[Install] %hs: failed: %hs", name.c_str(), outcome.message.c_str());
	}

	static ManageResult RefusalResult(const std::string& why)
	{
		return why == ModInstall::kVortexManaged ? ManageResult::Vortex : ManageResult::Failed;
	}

	// Ends an install: the card gets its result and another may start.
	static void EndInstall(const std::wstring& file, const ChangeOutcome& outcome)
	{
		EnterCriticalSection(&g_pluginLock);
		s_installing = false;
		RecordOutcome(file, ManageOp::Install, outcome);
		LeaveCriticalSection(&g_pluginLock);
	}

	void RequestPluginInstall(const ModIndex::Entry& entry, const std::vector<ModIndex::Entry>& list,
		bool listSigned, ModInstall::Installer& installer)
	{
		const std::wstring file = Widen(entry.install.dllFile);
		if (!g_managerInitialized || !ModIndex::detail::IsInstallDllName(entry.install.dllFile)) return;

		// Only a signed list may install.
		std::string why = g_startupComplete ? InstallBlockedReason(entry, list, GetManageSnapshot())
		                                    : "The plugins are still starting.";
		if (why.empty() && !listSigned)
			why = "This list isn't signed, so nothing can be installed from it.";

		EnterCriticalSection(&g_pluginLock);
		ManageState& state = s_manageStates[LowerCase(file)];
		if (state.working)
		{
			LeaveCriticalSection(&g_pluginLock);
			return;
		}
		state = { true, false, ManageOp::Install, ManageResult::None, {}, entry.install.dllFile, entry.name,
		          listSigned };
		if (why.empty() && s_installing)
			why = kAnotherInstall;
		const bool starting = why.empty();
		if (starting)
			s_installing = true;
		LeaveCriticalSection(&g_pluginLock);

		if (!starting)
		{
			const ChangeOutcome refused{ RefusalResult(why), why };
			LogInstall(entry.name, refused);
			RecordOutcome(file, ManageOp::Install, refused);
			return;
		}

		ModInstall::Request request;
		request.name = entry.name;
		request.version = entry.version;
		request.info = entry.install;
		request.pluginsDir = ManageFolders().plugins;
		request.gameDir = GameDir();

		// Called without g_pluginLock: the callback runs under the installer's
		// own lock and only hands the result to the game thread.
		const bool started = installer.Start(std::move(request),
			[file, name = entry.name, &installer](const ModInstall::Outcome& installed)
			{
				GameThreadDispatch::PostVoid([file, name, installed, &installer]
				{
					// A task queued before the installer was shut down must not load
					// into a loader that is tearing down.
					if (!g_managerInitialized || installer.Halted()) return;
					// The installer has logged why when it wrote nothing.
					ChangeOutcome outcome{ RefusalResult(installed.message), installed.message };
					if (installed.placed)
					{
						outcome = FinishInstall(file, installed.version);
						LogInstall(name, outcome);
					}
					RefreshManagedFiles();
					EndInstall(file, outcome);
				});
			});
		if (!started)
		{
			const ChangeOutcome refused{ ManageResult::Failed,
				"The install couldn't start. Restart the game and try again." };
			LogInstall(entry.name, refused);
			EndInstall(file, refused);
		}
	}
#endif

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
