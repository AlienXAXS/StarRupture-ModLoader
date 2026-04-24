#include "pch.h"
#include "experience_load_complete.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../ufunction_resolve.h"
#include "../scan_patterns.h"
#include "plugins/plugin_manager.h"
#include <vector>
#include <algorithm>

namespace Hooks::ExperienceLoadComplete
{
	// UCrExperienceManagerComponent::OnExperienceLoadComplete(UCrExperienceManagerComponent* this)
	using OnExperienceLoadComplete_t = void(__fastcall*)(void* thisPtr);

	static Hook g_hook;
	static OnExperienceLoadComplete_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive experience-load-complete events
	static std::vector<PluginExperienceLoadCompleteCallback> g_pluginCallbacks;

	// Late-registration state.
	static bool g_hasFired = false;

	static void __fastcall Detour(void* thisPtr)
	{
		void* const callerAddr = _ReturnAddress();

		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(
			L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   this=%p, Thread=%lu", thisPtr, GetCurrentThreadId());
		ModLoaderLogger::LogTrace(L"[ExperienceLoadComplete]   Called from: %S",
		                          Hooks::GetCallerModuleName(callerAddr).c_str());

		// Call original first so the experience is fully loaded before we notify plugins
		if (g_original)
		{
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   Calling original OnExperienceLoadComplete...");
			g_original(thisPtr);
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[ExperienceLoadComplete] Calling plugin callback #%zu (%S)",
				                          i + 1, Hooks::GetCallerModuleName((void*)g_pluginCallbacks[i]).c_str());

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] All plugin callbacks completed");
		}

		g_hasFired = true;
		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] OnExperienceLoadComplete complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] Installing hook...");

		uintptr_t addr = Hooks::ResolveUFunctionNativeAddr(
			"CrExperienceManagerComponent", "OnExperienceLoadComplete");
		if (!addr)
		{
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Not a UFUNCTION -- falling back to pattern scan");
			addr = Scanner::FindPatternInMainModule(
				"UCrExperienceManagerComponent::OnExperienceLoadComplete",
				ScanPatterns::UCrExperienceManagerComponent_OnExperienceLoadComplete);
		}
		if (!addr)
			return false;

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
		g_hasFired = false;
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginExperienceLoadCompleteCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[ExperienceLoadComplete] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] First callback registered � installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(
					L"[ExperienceLoadComplete] Failed to install hook for experience-load-complete callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Plugin callback registered (%zu total)",
		                          g_pluginCallbacks.size());

		// Late-registration replay: if already fired during startup, invoke immediately.
		if (g_hasFired && !PluginManager::IsStartupComplete())
		{
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Late-registration replay: firing callback immediately");
			try { callback(); }
			catch (...) { ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Exception in late-registration callback"); }
		}
	}

	void UnregisterPluginCallback(PluginExperienceLoadCompleteCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
