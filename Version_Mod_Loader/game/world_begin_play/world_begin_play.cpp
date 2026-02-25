#include "pch.h"
#include "world_begin_play.h"
#include "logger.h"
#include "scanner.h"
#include "../scan_patterns.h"
#include "../SDK.hpp"
#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"
#include <vector>
#include <algorithm>

namespace Hooks::WorldBeginPlay
{
	// UCrSessionWorldLoaderSubsystem::OnWorldBeginPlay signature
	// Uses proper SDK types
	typedef void(__fastcall* OnWorldBeginPlay_t)(SDK::UWorld* inWorld);

	static Hook g_hook;
	static OnWorldBeginPlay_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive world begin play events
	static std::vector<PluginWorldBeginPlayCallback> g_pluginCallbacks;
	static std::vector<PluginAnyWorldBeginPlayCallback> g_anyWorldCallbacks;

	static void __fastcall Detour(SDK::UWorld* inWorld)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		// Always log the world begin play event for debugging
		ModLoader::LogDebug(L"[WorldBeginPlay] World begin play detected (#%ld)", callNum);

		// Get world name
		std::string worldName;
		if (inWorld)
		{
			worldName = inWorld->GetName();
			ModLoader::LogDebug(L"[WorldBeginPlay]   World: %S", worldName.c_str());
		}

		// --- Notify any-world callbacks (fires for ALL worlds) ---
		if (!g_anyWorldCallbacks.empty())
		{
			ModLoader::LogDebug(L"[WorldBeginPlay] Notifying %zu any-world callback(s) for '%S'...",
				g_anyWorldCallbacks.size(), worldName.c_str());

			for (size_t i = 0; i < g_anyWorldCallbacks.size(); ++i)
			{
				if (!g_anyWorldCallbacks[i])
					continue;

				try
				{
					g_anyWorldCallbacks[i](inWorld, worldName.c_str());
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[WorldBeginPlay] Exception in any-world callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[WorldBeginPlay] Unknown exception in any-world callback");
				}
			}
		}

		// Check if this is the ChimeraMain world
		bool isChimeraWorld = worldName.find("ChimeraMain") != std::string::npos;

		if (!isChimeraWorld)
		{
			// Not ChimeraMain — call original and return (ChimeraMain callbacks not fired)
			ModLoader::LogInfo(L"[WorldBeginPlay]   Skipping ChimeraMain callbacks - not ChimeraMain world");
			if (g_original)
			{
				g_original(inWorld);
			}
			return;
		}

		// This is ChimeraMain - proceed with full logging and initialization
		ModLoader::LogInfo(L"[WorldBeginPlay] ChimeraMain world begin play detected (#%ld)", callNum);

		// Call original
		ModLoader::LogDebug(L"[WorldBeginPlay]   Calling original OnWorldBeginPlay...");
		if (g_original)
		{
			g_original(inWorld);
			ModLoader::LogDebug(L"[WorldBeginPlay]   Original returned");
		}
		else
		{
			ModLoader::LogError(L"[WorldBeginPlay] Original function pointer is null!");
		}

		// Notify ChimeraMain-only registered plugins (with error isolation)
		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[WorldBeginPlay] Notifying %zu ChimeraMain plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoader::LogTrace(L"[WorldBeginPlay]   Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i](inWorld);
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[WorldBeginPlay] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[WorldBeginPlay] Unknown exception in callback");
				}
			}

			ModLoader::LogDebug(L"[WorldBeginPlay] All plugin callbacks completed");
		}

		ModLoader::LogDebug(L"[WorldBeginPlay] OnWorldBeginPlay complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[WorldBeginPlay] Installing hook...");

		// Pattern sourced from scan_patterns.h (differs between client and server builds)
		const char* pattern = ScanPatterns::UWorld_BeginPlay;

		ModLoader::LogInfo(L"[WorldBeginPlay] Scanning for OnWorldBeginPlay...");
		ModLoader::LogDebug(L"[WorldBeginPlay]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[WorldBeginPlay] OnWorldBeginPlay pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogDebug(L"[WorldBeginPlay] OnWorldBeginPlay found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[WorldBeginPlay] Hook installed successfully (filtering for ChimeraMain worlds)");
		else
			ModLoader::LogError(L"[WorldBeginPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[WorldBeginPlay] Removing hook...");
		g_hook.Remove();

		// Clear all callbacks
		g_pluginCallbacks.clear();
		g_anyWorldCallbacks.clear();
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
			ModLoader::LogWarn(L"[WorldBeginPlay] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[WorldBeginPlay] First ChimeraMain callback registered — installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[WorldBeginPlay] Failed to install hook for ChimeraMain callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[WorldBeginPlay] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginWorldBeginPlayCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[WorldBeginPlay] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}

	void RegisterAnyWorldCallback(PluginAnyWorldBeginPlayCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[WorldBeginPlay] RegisterAnyWorldCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first any-world registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[WorldBeginPlay] First any-world callback registered — installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[WorldBeginPlay] Failed to install hook for any-world callback!");
				return;
			}
		}

		g_anyWorldCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[WorldBeginPlay] Any-world callback registered (%zu total)", g_anyWorldCallbacks.size());
	}

	void UnregisterAnyWorldCallback(PluginAnyWorldBeginPlayCallback callback)
	{
		auto it = std::find(g_anyWorldCallbacks.begin(), g_anyWorldCallbacks.end(), callback);
		if (it != g_anyWorldCallbacks.end())
		{
			g_anyWorldCallbacks.erase(it);
			ModLoader::LogDebug(L"[WorldBeginPlay] Any-world callback unregistered (%zu remaining)", g_anyWorldCallbacks.size());
		}
	}
}
