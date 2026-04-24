#include "pch.h"
#include "world_begin_play.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../ufunction_resolve.h"
#include "../scan_patterns.h"
#include "../SDK.hpp"
#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"
#include "plugins/plugin_manager.h"
#include <vector>
#include <algorithm>

namespace Hooks::WorldBeginPlay
{
	// UCrSessionWorldLoaderSubsystem::OnWorldBeginPlay()
	// x64 __fastcall member function: RCX = this (subsystem) only.
	// There is NO UWorld* parameter -- the subsystem fetches the world internally.
	// The world is retrieved via SDK::UWorld::GetWorld() after the original runs.
	using OnWorldBeginPlay_t = void(__fastcall*)(void* subsystemThis);

	static Hook g_hook;
	static OnWorldBeginPlay_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive world begin play events
	static std::vector<PluginWorldBeginPlayCallback> g_pluginCallbacks;
	static std::vector<PluginAnyWorldBeginPlayCallback> g_anyWorldCallbacks;

	// Late-registration state: track last worlds so callbacks registered
	// during startup (after the hook fired) still receive the event.
	static bool            g_chimeraHasFired  = false;
	static SDK::UWorld*    g_lastChimeraWorld = nullptr;
	static bool            g_anyWorldHasFired = false;
	static SDK::UWorld*    g_lastAnyWorld     = nullptr;
	static std::string     g_lastAnyWorldName;

	static void __fastcall Detour(void* subsystemThis)
	{
		// Capture return address before any other calls alter the stack frame.
		void* const callerAddr = _ReturnAddress();

		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogDebug(L"[WorldBeginPlay] World begin play detected (#%ld) subsystem=%p Thread=%lu",
		                          callNum, subsystemThis, GetCurrentThreadId());
		ModLoaderLogger::LogTrace(L"[WorldBeginPlay]   Called from: %S",
		                          Hooks::GetCallerModuleName(callerAddr).c_str());

		// Call original first so the world is fully initialised before we read from it.
		ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   Calling original OnWorldBeginPlay...");
		if (g_original)
		{
			g_original(subsystemThis);
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[WorldBeginPlay] Original function pointer is null!");
		}

		// Fetch the world via the engine's own getter now that the original has run.
		auto* inWorld = static_cast<SDK::UWorld*>(SDK::UWorld::GetWorld());
		std::string worldName;
		if (inWorld)
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   inWorld=%p, reading name...", inWorld);
			worldName = inWorld->GetName();
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   World: %S", worldName.c_str());
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[WorldBeginPlay]   GetWorld() returned null after original ran");
		}

		// Record any-world fired state for late-registration replay.
		g_anyWorldHasFired = true;
		g_lastAnyWorld     = inWorld;
		g_lastAnyWorldName = worldName;

		// --- Notify any-world callbacks (fires for ALL worlds) ---
		if (!g_anyWorldCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Notifying %zu any-world callback(s) for '%S'...",
			                          g_anyWorldCallbacks.size(), worldName.c_str());

			for (size_t i = 0; i < g_anyWorldCallbacks.size(); ++i)
			{
				if (!g_anyWorldCallbacks[i])
					continue;

				try
				{
					ModLoaderLogger::LogTrace(L"[WorldBeginPlay] Calling any-world callback #%zu (%S) for world '%S'",
					                          i + 1, Hooks::GetCallerModuleName((void*)g_anyWorldCallbacks[i]).c_str(), worldName.c_str());
					g_anyWorldCallbacks[i](inWorld, worldName.c_str());
					ModLoaderLogger::LogTrace(L"[WorldBeginPlay] Finished any-world callback #%zu", i + 1);
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[WorldBeginPlay] Exception in any-world callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[WorldBeginPlay] Unknown exception in any-world callback");
				}
			}
		}

		// Check if this is the ChimeraMain world
		bool isChimeraWorld = worldName.find("ChimeraMain") != std::string::npos;

		if (!isChimeraWorld)
		{
			ModLoaderLogger::LogInfo(L"[WorldBeginPlay]   Not ChimeraMain world - skipping ChimeraMain callbacks");
			return;
		}

		// Record ChimeraMain fired state for late-registration replay.
		g_chimeraHasFired  = true;
		g_lastChimeraWorld = inWorld;

		// This is ChimeraMain - notify registered plugins
		ModLoaderLogger::LogInfo(L"[WorldBeginPlay] ChimeraMain world begin play detected (#%ld)", callNum);

		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Notifying %zu ChimeraMain plugin(s)...",
			                          g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[WorldBeginPlay]   Calling ChimeraMain callback #%zu (%S)",
				                          i + 1, Hooks::GetCallerModuleName((void*)g_pluginCallbacks[i]).c_str());

				try
				{
					g_pluginCallbacks[i](inWorld);
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[WorldBeginPlay] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[WorldBeginPlay] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] All plugin callbacks completed");
		}

		ModLoaderLogger::LogDebug(L"[WorldBeginPlay] OnWorldBeginPlay complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[WorldBeginPlay] Installing hook...");

		uintptr_t addr = Hooks::ResolveUFunctionNativeAddr("World", "BeginPlay");
		if (!addr)
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Not a UFUNCTION -- falling back to pattern scan");
			addr = Scanner::FindPatternInMainModule(
				"UWorld::BeginPlay",
				ScanPatterns::UWorld_BeginPlay);
		}
		if (!addr)
			return false;

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[WorldBeginPlay] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[WorldBeginPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[WorldBeginPlay] Removing hook...");
		g_hook.Remove();

		g_pluginCallbacks.clear();
		g_anyWorldCallbacks.clear();
		g_chimeraHasFired  = false;
		g_lastChimeraWorld = nullptr;
		g_anyWorldHasFired = false;
		g_lastAnyWorld     = nullptr;
		g_lastAnyWorldName.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	long GetCallCount()
	{
		return g_callCount;
	}

	void RegisterPluginCallback(PluginWorldBeginPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[WorldBeginPlay] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(
				L"[WorldBeginPlay] First ChimeraMain callback registered � installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[WorldBeginPlay] Failed to install hook for ChimeraMain callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Plugin callback registered (%zu total)", g_pluginCallbacks.size());

		// Late-registration replay: if ChimeraMain already fired during startup,
		// invoke immediately so plugins initialized after the event don't miss it.
		// Skipped for runtime hot-reloads (startup is complete by then).
		if (g_chimeraHasFired && g_lastChimeraWorld && !PluginManager::IsStartupComplete())
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Late-registration replay: firing ChimeraMain callback immediately");
			try { callback(g_lastChimeraWorld); }
			catch (...) { ModLoaderLogger::LogError(L"[WorldBeginPlay] Exception in late-registration ChimeraMain callback"); }
		}
	}

	void UnregisterPluginCallback(PluginWorldBeginPlayCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	void RegisterAnyWorldCallback(PluginAnyWorldBeginPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[WorldBeginPlay] RegisterAnyWorldCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first any-world registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[WorldBeginPlay] First any-world callback registered � installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[WorldBeginPlay] Failed to install hook for any-world callback!");
				return;
			}
		}

		g_anyWorldCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Any-world callback registered (%zu total)",
		                          g_anyWorldCallbacks.size());

		// Late-registration replay: if any world fired during startup, invoke now.
		if (g_anyWorldHasFired && g_lastAnyWorld && !PluginManager::IsStartupComplete())
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Late-registration replay: firing any-world callback immediately for '%S'",
			                          g_lastAnyWorldName.c_str());
			try { callback(g_lastAnyWorld, g_lastAnyWorldName.c_str()); }
			catch (...) { ModLoaderLogger::LogError(L"[WorldBeginPlay] Exception in late-registration any-world callback"); }
		}
	}

	void UnregisterAnyWorldCallback(PluginAnyWorldBeginPlayCallback callback)
	{
		auto it = std::find(g_anyWorldCallbacks.begin(), g_anyWorldCallbacks.end(), callback);
		if (it != g_anyWorldCallbacks.end())
		{
			g_anyWorldCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Any-world callback unregistered (%zu remaining)",
			                          g_anyWorldCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
