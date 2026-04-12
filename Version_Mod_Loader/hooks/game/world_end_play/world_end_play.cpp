#include "pch.h"
#include "world_end_play.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../SDK.hpp"
#include "Engine_classes.hpp"
#include <vector>
#include <algorithm>

namespace Hooks::WorldEndPlay
{
	// UWorld::EndPlay()
	// x64 __fastcall member function: RCX = this (UWorld*)
	using OnWorldEndPlay_t = void(__fastcall*)(void* worldThis);

	static Hook g_hook;
	static OnWorldEndPlay_t g_original = nullptr;
	static long g_callCount = 0;

	static std::vector<PluginWorldEndPlayCallback> g_beforeCallbacks;
	static std::vector<PluginWorldEndPlayCallback> g_afterCallbacks;

	static void __fastcall Detour(void* worldThis)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogDebug(L"[WorldEndPlay] World end play detected (#%ld) world=%p Thread=%lu",
		                          callNum, worldThis, GetCurrentThreadId());

		// Fetch world name before the original runs (world is still valid here).
		auto* inWorld = static_cast<SDK::UWorld*>(worldThis);
		std::string worldName;
		if (inWorld)
		{
			worldName = inWorld->GetName();
			ModLoaderLogger::LogDebug(L"[WorldEndPlay]   World: %S", worldName.c_str());
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[WorldEndPlay]   worldThis is null");
		}

		// --- Before callbacks ---
		if (!g_beforeCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Notifying %zu before callback(s)...",
			                          g_beforeCallbacks.size());

			for (size_t i = 0; i < g_beforeCallbacks.size(); ++i)
			{
				if (!g_beforeCallbacks[i])
					continue;

				try
				{
					g_beforeCallbacks[i](inWorld, worldName.c_str());
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[WorldEndPlay] Exception in before callback #%zu: %S", i + 1, e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[WorldEndPlay] Unknown exception in before callback #%zu", i + 1);
				}
			}
		}

		// --- Call original ---
		ModLoaderLogger::LogDebug(L"[WorldEndPlay]   Calling original UWorld::EndPlay...");
		if (g_original)
		{
			g_original(worldThis);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] Original function pointer is null!");
		}

		// --- After callbacks ---
		if (!g_afterCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Notifying %zu after callback(s)...",
			                          g_afterCallbacks.size());

			for (size_t i = 0; i < g_afterCallbacks.size(); ++i)
			{
				if (!g_afterCallbacks[i])
					continue;

				try
				{
					g_afterCallbacks[i](inWorld, worldName.c_str());
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[WorldEndPlay] Exception in after callback #%zu: %S", i + 1, e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[WorldEndPlay] Unknown exception in after callback #%zu", i + 1);
				}
			}
		}

		ModLoaderLogger::LogDebug(L"[WorldEndPlay] EndPlay complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[WorldEndPlay] Installing hook...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"UWorld::EndPlay",
			ScanPatterns::UWorld_EndPlay);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] Pattern scan failed");
			return false;
		}

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[WorldEndPlay] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[WorldEndPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[WorldEndPlay] Removing hook...");
		g_hook.Remove();
		g_beforeCallbacks.clear();
		g_afterCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	long GetCallCount()
	{
		return g_callCount;
	}

	void RegisterBeforeCallback(PluginWorldEndPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[WorldEndPlay] RegisterBeforeCallback: null callback provided");
			return;
		}

		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[WorldEndPlay] First before callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[WorldEndPlay] Failed to install hook for before callback!");
				return;
			}
		}

		g_beforeCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[WorldEndPlay] Before callback registered (%zu total)", g_beforeCallbacks.size());
	}

	void UnregisterBeforeCallback(PluginWorldEndPlayCallback callback)
	{
		auto it = std::find(g_beforeCallbacks.begin(), g_beforeCallbacks.end(), callback);
		if (it != g_beforeCallbacks.end())
		{
			g_beforeCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Before callback unregistered (%zu remaining)",
			                          g_beforeCallbacks.size());
		}
	}

	void RegisterAfterCallback(PluginWorldEndPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[WorldEndPlay] RegisterAfterCallback: null callback provided");
			return;
		}

		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[WorldEndPlay] First after callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[WorldEndPlay] Failed to install hook for after callback!");
				return;
			}
		}

		g_afterCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[WorldEndPlay] After callback registered (%zu total)", g_afterCallbacks.size());
	}

	void UnregisterAfterCallback(PluginWorldEndPlayCallback callback)
	{
		auto it = std::find(g_afterCallbacks.begin(), g_afterCallbacks.end(), callback);
		if (it != g_afterCallbacks.end())
		{
			g_afterCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] After callback unregistered (%zu remaining)",
			                          g_afterCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
