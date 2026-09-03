#include "pch.h"
#include "save_loaded.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../ufunction_resolve.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::SaveLoaded
{
	// Two different functions can end up behind this hook, with two different ABIs:
	//
	//   1. UCrMassSaveSubsystem::execOnSaveLoaded -- the UHT-generated exec thunk.
	//      This is what UFunction::ExecFunction points at, so it is what
	//      ResolveUFunctionNativeAddr returns, and OnSaveLoaded is reached this way
	//      in practice (it is bound as a dynamic delegate, so the engine enters it
	//      through ProcessEvent -> UFunction::Invoke). Its signature is the
	//      FNativeFuncPtr shape: (UObject* Context, FFrame& Stack, void* Result).
	//   2. UCrMassSaveSubsystem::OnSaveLoaded -- the real native member, found by
	//      the pattern-scan fallback. RCX = this, nothing else.
	//
	// So the detour declares the three-argument exec shape and forwards all three
	// registers. That is required for (1) and harmless for (2): the native member
	// never reads RDX/R8, and the arguments are register-only so there is no stack
	// to clean up either way.
	//
	// Dropping RDX/R8 is NOT harmless in the other direction, and shipped as a
	// crash: every exec thunk starts with P_FINISH, which writes through
	// FFrame& Stack. Forwarding only `this` left RDX holding whatever the logging
	// calls just above had put there, and execOnSaveLoaded faulted writing to it
	// (EXCEPTION_ACCESS_VIOLATION into this DLL's own image).
	using OnSaveLoaded_t = void(__fastcall*)(void* context, void* stack, void* result);

	static Hook g_hook;
	static OnSaveLoaded_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive save-loaded events
	static std::vector<PluginSaveLoadedCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* context, void* stack, void* result)
	{
		void* const callerAddr = _ReturnAddress();

		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[SaveLoaded]   this=%p, Thread=%lu", context, GetCurrentThreadId());
		ModLoaderLogger::LogTrace(L"[SaveLoaded]   Called from: %S",
		                          Hooks::GetCallerModuleName(callerAddr).c_str());

		// Call original first so the save is fully loaded before we notify plugins
		if (g_original)
		{
			ModLoaderLogger::LogDebug(L"[SaveLoaded]   Calling original OnSaveLoaded...");
			g_original(context, stack, result);
			ModLoaderLogger::LogDebug(L"[SaveLoaded]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[SaveLoaded] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[SaveLoaded] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[SaveLoaded] Calling plugin callback #%zu (%S)",
				                          i + 1, Hooks::GetCallerModuleName((void*)g_pluginCallbacks[i]).c_str());

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[SaveLoaded] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[SaveLoaded] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[SaveLoaded] All plugin callbacks completed");
		}

		ModLoaderLogger::LogDebug(L"[SaveLoaded] OnSaveLoaded complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[SaveLoaded] Installing hook...");

		uintptr_t addr = Hooks::ResolveUFunctionNativeAddr("CrMassSaveSubsystem", "OnSaveLoaded");
		if (!addr)
		{
			ModLoaderLogger::LogDebug(L"[SaveLoaded] Not a UFUNCTION -- falling back to pattern scan");
			addr = Scanner::FindPatternInMainModule(
				"UCrMassSaveSubsystem::OnSaveLoaded",
				ScanPatterns::UCrMassSaveSubsystem_OnSaveLoaded);
		}
		if (!addr)
			return false;

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[SaveLoaded] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[SaveLoaded] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[SaveLoaded] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginSaveLoadedCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[SaveLoaded] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[SaveLoaded] First callback registered -- installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[SaveLoaded] Failed to install hook for save-loaded callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[SaveLoaded] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginSaveLoadedCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[SaveLoaded] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
