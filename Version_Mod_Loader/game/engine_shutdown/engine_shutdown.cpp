#include "pch.h"
#include "engine_shutdown.h"
#include "../../logger.h"
#include "../../scanner.h"
#include <vector>
#include <algorithm>

namespace Hooks::EngineShutdown
{
	// Hook 1: FEngineLoop::Exit (primary)
	typedef void(__fastcall* FEngineLoop_Exit_t)(void* thisPtr);
	static Hook g_engineLoopExitHook;
	static FEngineLoop_Exit_t g_engineLoopExitOriginal = nullptr;

	// Hook 2: UEngine::PreExit (fallback) - fires before GC flushes objects
	typedef void(__fastcall* UEngine_PreExit_t)(void* thisPtr);
	static Hook g_enginePreExitHook;
	static UEngine_PreExit_t g_enginePreExitOriginal = nullptr;

	// Only notify plugins once regardless of which hook fires first
	static bool g_shutdownFired = false;

	// Registered plugin callbacks
	static std::vector<PluginEngineShutdownCallback> g_pluginCallbacks;

	static void NotifyEngineShuttingDown(const wchar_t* hookSource)
	{
		if (g_shutdownFired)
			return;

		g_shutdownFired = true;

		ModLoader::LogInfo(L"[EngineShutdown] *** ENGINE SHUTTING DOWN *** (via %s) - notifying plugins", hookSource);

		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (!g_pluginCallbacks[i])
				continue;

			ModLoader::LogTrace(L"[EngineShutdown]   Calling plugin callback #%zu", i + 1);
			try
			{
				g_pluginCallbacks[i]();
			}
			catch (const std::exception& e)
			{
				ModLoader::LogError(L"[EngineShutdown] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoader::LogError(L"[EngineShutdown] Unknown exception in callback");
			}
		}

		ModLoader::LogDebug(L"[EngineShutdown] All plugin callbacks completed");
	}

	// Hook 1: FEngineLoop::Exit detour
	static void __fastcall FEngineLoop_Exit_Detour(void* thisPtr)
	{
		ModLoader::LogInfo(L"[EngineShutdown] FEngineLoop::Exit called");

		// Notify plugins BEFORE calling original so UObject system is still intact
		NotifyEngineShuttingDown(L"FEngineLoop::Exit");

		if (g_engineLoopExitOriginal)
			g_engineLoopExitOriginal(thisPtr);
	}

	// Hook 2: UEngine::PreExit detour
	static void __fastcall UEngine_PreExit_Detour(void* thisPtr)
	{
		ModLoader::LogInfo(L"[EngineShutdown] UEngine::PreExit called");

		// Notify plugins BEFORE calling original
		NotifyEngineShuttingDown(L"UEngine::PreExit");

		if (g_enginePreExitOriginal)
			g_enginePreExitOriginal(thisPtr);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[EngineShutdown] Installing engine shutdown hooks...");

		bool anyHookSucceeded = false;
		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		// Hook 1: FEngineLoop::Exit (primary)
		{
			const char* pattern =
				"40 53 55 56 57 48 83 EC ?? ?? ?? ?? 48 8B F9 FF 50";

			ModLoader::LogInfo(L"[EngineShutdown] Scanning for FEngineLoop::Exit...");
			ModLoader::LogDebug(L"[EngineShutdown]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule(pattern);
			if (addr)
			{
				ModLoader::LogInfo(L"[EngineShutdown] ✓ FEngineLoop::Exit found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_engineLoopExitHook.Install(
					addr,
					reinterpret_cast<void*>(&FEngineLoop_Exit_Detour),
					reinterpret_cast<void**>(&g_engineLoopExitOriginal));

				if (hookOk)
				{
					ModLoader::LogInfo(L"[EngineShutdown] ✓ FEngineLoop::Exit hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoader::LogWarn(L"[EngineShutdown] ✗ FEngineLoop::Exit hook installation failed");
				}
			}
			else
			{
				ModLoader::LogWarn(L"[EngineShutdown] ✗ FEngineLoop::Exit pattern not found - will try fallback");
			}
		}

		// Hook 2: UEngine::PreExit (fallback)
		{
			const char* pattern =
				"E8 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? ?? 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 15";

			ModLoader::LogInfo(L"[EngineShutdown] Scanning for UEngine::PreExit (fallback)...");
			ModLoader::LogDebug(L"[EngineShutdown]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule(pattern);
			if (addr)
			{
				ModLoader::LogInfo(L"[EngineShutdown] ✓ UEngine::PreExit found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_enginePreExitHook.Install(
					addr,
					reinterpret_cast<void*>(&UEngine_PreExit_Detour),
					reinterpret_cast<void**>(&g_enginePreExitOriginal));

				if (hookOk)
				{
					ModLoader::LogInfo(L"[EngineShutdown] ✓ UEngine::PreExit hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoader::LogWarn(L"[EngineShutdown] ✗ UEngine::PreExit hook installation failed");
				}
			}
			else
			{
				ModLoader::LogWarn(L"[EngineShutdown] ✗ UEngine::PreExit pattern not found");
			}
		}

		if (anyHookSucceeded)
			ModLoader::LogInfo(L"[EngineShutdown] At least one shutdown hook installed - engine shutdown detection active");
		else
			ModLoader::LogError(L"[EngineShutdown] CRITICAL: No shutdown hooks installed - plugins will NOT receive shutdown callbacks!");

		return anyHookSucceeded;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[EngineShutdown] Removing engine shutdown hooks...");
		g_engineLoopExitHook.Remove();
		g_enginePreExitHook.Remove();
		g_pluginCallbacks.clear();
		ModLoader::LogInfo(L"[EngineShutdown] All hooks removed");
	}

	void RegisterPluginCallback(PluginEngineShutdownCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[EngineShutdown] RegisterPluginCallback: null callback");
			return;
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[EngineShutdown] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginEngineShutdownCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[EngineShutdown] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
