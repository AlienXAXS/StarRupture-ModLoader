#include "pch.h"
#include "world_begin_play.h"
#include "logging/logger.h"
#include "plugins/plugin_guard.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../SDK.hpp"
#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"
#include "plugins/plugin_manager.h"
#include "plugins/reload_policy.h"
#include "hooks/game/engine_init/engine_init.h"
#include <vector>
#include <algorithm>

namespace Hooks::WorldBeginPlay
{
	// UWorld::BeginPlay()
	// x64 __fastcall member function: RCX = this, the world beginning play.
	// That world is the one reported, not a global getter's: another world
	// alive at the time (a plugin's preview or capture world) must not be
	// taken for it.
	using OnWorldBeginPlay_t = void(__fastcall*)(SDK::UWorld* world);

	static Hook g_hook;
	static OnWorldBeginPlay_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive world begin play events
	static std::vector<PluginWorldBeginPlayCallback> g_pluginCallbacks;
	static std::vector<PluginAnyWorldBeginPlayCallback> g_anyWorldCallbacks;

	// Late-registration state: track last worlds so callbacks registered
	// during startup (after the hook fired) still receive the event.
	static bool            g_mainMenuFallbackFired = false;
	static bool            g_chimeraHasFired  = false;
	static SDK::UWorld*    g_lastChimeraWorld = nullptr;
	static bool            g_anyWorldHasFired = false;
	static SDK::UWorld*    g_lastAnyWorld     = nullptr;
	static std::string     g_lastAnyWorldName;

	static void __fastcall Detour(SDK::UWorld* world)
	{
		// Capture return address before any other calls alter the stack frame.
		void* const callerAddr = _ReturnAddress();

		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogDebug(L"[WorldBeginPlay] World begin play detected (#%ld) world=%p Thread=%lu",
		                          callNum, world, GetCurrentThreadId());
		ModLoaderLogger::LogTrace(L"[WorldBeginPlay]   Called from: %S",
		                          Hooks::GetCallerModuleName(callerAddr).c_str());

		// Call original first so the world is fully initialised before we read from it.
		ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   Calling original UWorld::BeginPlay...");
		if (g_original)
		{
			g_original(world);
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[WorldBeginPlay] Original function pointer is null!");
		}

		// The world this BeginPlay was for, read now that the original has run.
		SDK::UWorld* const inWorld = world;
		std::string worldName;
		if (inWorld)
		{
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   inWorld=%p, reading name...", inWorld);
			worldName = inWorld->GetName();
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay]   World: %S", worldName.c_str());
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[WorldBeginPlay]   BeginPlay was called with no world");
		}

		// Whether a gameplay world is running, for the plugins' load and unload settings.
		ReloadPolicy::NoteWorldBegan(worldName.c_str());

		// Record any-world fired state for late-registration replay.
		g_anyWorldHasFired = true;
		g_lastAnyWorld     = inWorld;
		g_lastAnyWorldName = worldName;

		// If the engine-init hooks never fired, Map_MainMenu proves the engine is ready.
		if (!g_mainMenuFallbackFired && worldName.find("Map_MainMenu") != std::string::npos)
		{
			g_mainMenuFallbackFired = true;
			ModLoaderLogger::LogWarn(L"[WorldBeginPlay] Map_MainMenu detected -- triggering engine-ready fallback");
			Hooks::EngineInit::TriggerEngineReadyFallback(L"Map_MainMenu WorldBeginPlay fallback");
		}

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
					PluginGuard::Invoke(g_anyWorldCallbacks[i], "any-world begin-play callback", inWorld, worldName.c_str());
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
		bool isChimeraWorld = ReloadPolicy::IsGameplayWorldName(worldName.c_str());

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
					PluginGuard::Invoke(g_pluginCallbacks[i], "world begin-play callback", inWorld);
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

		// UWorld::BeginPlay is not a UFUNCTION, so there is no UFunction to
		// resolve, and the hook must be in place before the first world starts.
		uintptr_t addr = Scanner::FindPatternInMainModule(
			"UWorld::BeginPlay",
			ScanPatterns::UWorld_BeginPlay);
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
			try { PluginGuard::Invoke(callback, "world begin-play callback", g_lastChimeraWorld); }
			catch (...) { ModLoaderLogger::LogError(L"[WorldBeginPlay] Exception in late-registration ChimeraMain callback"); }
		}
		else if (g_chimeraHasFired && PluginManager::IsStartupComplete())
		{
			PluginManager::NoteMissedEvent(PluginManager::MissedChimeraMain);
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
			try { PluginGuard::Invoke(callback, "any-world begin-play callback", g_lastAnyWorld, g_lastAnyWorldName.c_str()); }
			catch (...) { ModLoaderLogger::LogError(L"[WorldBeginPlay] Exception in late-registration any-world callback"); }
		}
		else if (g_anyWorldHasFired && PluginManager::IsStartupComplete())
		{
			PluginManager::NoteMissedEvent(PluginManager::MissedAnyWorld);
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

	// Drops every callback that lives in `module`: a plugin being unloaded,
	// whatever it did not unregister itself. Game thread, before FreeLibrary.
	void ForgetModule(HMODULE module)
	{
		const size_t dropped = PluginGuard::ForgetModule(g_pluginCallbacks, module)
		                      + PluginGuard::ForgetModule(g_anyWorldCallbacks, module);
		if (dropped)
			ModLoaderLogger::LogDebug(L"[WorldBeginPlay] Dropped %zu callback(s) of an unloading plugin", dropped);
	}
}
