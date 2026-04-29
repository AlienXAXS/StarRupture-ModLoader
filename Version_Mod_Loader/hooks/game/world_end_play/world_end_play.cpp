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
	// UWorld::EndPlay(EEndPlayReason::Type)
	// x64 __fastcall member function: RCX = this (UWorld*), RDX = reason (uint8 enum)
	using OnWorldEndPlay_t = char(__fastcall*)(void* worldThis, int endPlayReason);

	static Hook g_hook;
	static OnWorldEndPlay_t g_original = nullptr;
	static long g_callCount = 0;

	static std::vector<PluginWorldEndPlayCallback> g_beforeCallbacks;
	static std::vector<PluginWorldEndPlayCallback> g_afterCallbacks;

	// SEH wrapper — must be in its own function with no C++ objects to satisfy C2712.
	static void InvokeCallbackSEH(PluginWorldEndPlayCallback cb, SDK::UWorld* world, const char* name, size_t idx, const wchar_t* phase)
	{
		__try
		{
			cb(world, name);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] SEH exception in %s callback #%zu (code=0x%08X)", phase, idx + 1, GetExceptionCode());
		}
	}

	// SEH wrapper for original function — must be in its own function with no C++ objects to satisfy C2712.
	static void CallOriginalSEH(void* worldThis, int endPlayReason)
	{
		__try
		{
			g_original(worldThis, endPlayReason);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] SEH exception in original UWorld::EndPlay (code=0x%08X)", GetExceptionCode());
		}
	}

	static char __fastcall Detour(void* worldThis, int endPlayReason)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogDebug(L"[WorldEndPlay] World end play detected (#%ld) world=%p Thread=%lu",
		                          callNum, worldThis, GetCurrentThreadId());

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
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Notifying %zu before callback(s)...", g_beforeCallbacks.size());
			for (size_t i = 0; i < g_beforeCallbacks.size(); ++i)
			{
				if (g_beforeCallbacks[i])
					InvokeCallbackSEH(g_beforeCallbacks[i], inWorld, worldName.c_str(), i, L"before");
			}
		}

		// --- Call original ---
		ModLoaderLogger::LogDebug(L"[WorldEndPlay]   Calling original UWorld::EndPlay (reason=%d)...", endPlayReason);
		if (g_original)
		{
			CallOriginalSEH(inWorld, endPlayReason);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] Original function pointer is null!");
		}

		// --- After callbacks ---
		if (!g_afterCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Notifying %zu after callback(s)...", g_afterCallbacks.size());
			for (size_t i = 0; i < g_afterCallbacks.size(); ++i)
			{
				if (g_afterCallbacks[i])
					InvokeCallbackSEH(g_afterCallbacks[i], inWorld, worldName.c_str(), i, L"after");
			}
		}

		ModLoaderLogger::LogDebug(L"[WorldEndPlay] EndPlay complete (#%ld)", callNum);
		return 0; // Return value is ignored by the caller, so we can return anything.
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
