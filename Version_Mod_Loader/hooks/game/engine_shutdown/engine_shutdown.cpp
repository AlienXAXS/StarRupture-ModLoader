#include "pch.h"
#include "engine_shutdown.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "hooks/memory/engine_allocator.h"
#include <vector>
#include <algorithm>
#include "../scan_patterns.h"

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
	static bool g_shutdownInProgress = false;  // Add flag to track if we're cleaning up

	// Registered plugin callbacks
	static std::vector<PluginEngineShutdownCallback> g_pluginCallbacks;

	static void NotifyEngineShuttingDown(const wchar_t* hookSource)
	{
		// Don't fire if we're already in cleanup mode (callbacks cleared)
		if (g_shutdownInProgress)
		{
			ModLoaderLogger::LogWarn(L"[EngineShutdown] Hook fired during cleanup - ignoring (source: %s)", hookSource);
			return;
		}

		if (g_shutdownFired)
			return;

		g_shutdownFired = true;

		ModLoaderLogger::LogInfo(L"[EngineShutdown] *** ENGINE SHUTTING DOWN *** (via %s) - notifying plugins", hookSource);

		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (!g_pluginCallbacks[i])
				continue;

			ModLoaderLogger::LogTrace(L"[EngineShutdown]   Calling plugin callback #%zu", i + 1);
			try
			{
				g_pluginCallbacks[i]();
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[EngineShutdown] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[EngineShutdown] Unknown exception in callback");
			}
		}

		ModLoaderLogger::LogDebug(L"[EngineShutdown] All plugin callbacks completed");

		// Shut down the engine allocator after all plugins have cleaned up.
		// This must come after plugin callbacks since they may need to free
		// engine-allocated memory during their shutdown.
		EngineAllocator::Shutdown();
	}

	// Hook 1: FEngineLoop::Exit detour
	static void __fastcall FEngineLoop_Exit_Detour(void* thisPtr)
	{
		ModLoaderLogger::LogInfo(L"[EngineShutdown] FEngineLoop::Exit called");

		// Notify plugins BEFORE calling original so UObject system is still intact
		NotifyEngineShuttingDown(L"FEngineLoop::Exit");

		if (g_engineLoopExitOriginal)
			g_engineLoopExitOriginal(thisPtr);
	}

	// Hook 2: UEngine::PreExit detour
	static void __fastcall UEngine_PreExit_Detour(void* thisPtr)
	{
		ModLoaderLogger::LogInfo(L"[EngineShutdown] UEngine::PreExit called");

		// Notify plugins BEFORE calling original
		NotifyEngineShuttingDown(L"UEngine::PreExit");

		if (g_enginePreExitOriginal)
			g_enginePreExitOriginal(thisPtr);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[EngineShutdown] Installing engine shutdown hooks...");

		bool anyHookSucceeded = false;
		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		// Hook 1: FEngineLoop::Exit (primary)
		{
			const char* pattern = ScanPatterns::FEngineLoop_Exit;
				

			ModLoaderLogger::LogInfo(L"[EngineShutdown] Scanning for FEngineLoop::Exit...");
			ModLoaderLogger::LogDebug(L"[EngineShutdown]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule("FEngineLoop::Exit", pattern);
			if (addr)
			{
				ModLoaderLogger::LogDebug(L"[EngineShutdown] [OK] FEngineLoop::Exit found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_engineLoopExitHook.Install(
					addr,
					reinterpret_cast<void*>(&FEngineLoop_Exit_Detour),
					reinterpret_cast<void**>(&g_engineLoopExitOriginal));

				if (hookOk)
				{
					ModLoaderLogger::LogInfo(L"[EngineShutdown] [OK] FEngineLoop::Exit hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoaderLogger::LogWarn(L"[EngineShutdown] [FAIL] FEngineLoop::Exit hook installation failed");
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[EngineShutdown] [FAIL] FEngineLoop::Exit pattern not found - will try fallback");
			}
		}

		// Hook 2: UEngine::PreExit (fallback)
		{
			const char* pattern = ScanPatterns::UEngine_PreExit;

			ModLoaderLogger::LogInfo(L"[EngineShutdown] Scanning for UEngine::PreExit (fallback)...");
			ModLoaderLogger::LogDebug(L"[EngineShutdown]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule("UEngine::PreExit", pattern);
			if (addr)
			{
				ModLoaderLogger::LogDebug(L"[EngineShutdown] [OK] UEngine::PreExit found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_enginePreExitHook.Install(
					addr,
					reinterpret_cast<void*>(&UEngine_PreExit_Detour),
					reinterpret_cast<void**>(&g_enginePreExitOriginal));

				if (hookOk)
				{
					ModLoaderLogger::LogInfo(L"[EngineShutdown] [OK] UEngine::PreExit hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoaderLogger::LogWarn(L"[EngineShutdown] [FAIL] UEngine::PreExit hook installation failed");
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[EngineShutdown] [FAIL] UEngine::PreExit pattern not found");
			}
		}

		if (anyHookSucceeded)
			ModLoaderLogger::LogDebug(L"[EngineShutdown] At least one shutdown hook installed - engine shutdown detection active");
		else
			ModLoaderLogger::LogError(L"[EngineShutdown] CRITICAL: No shutdown hooks installed - plugins will NOT receive shutdown callbacks!");

		return anyHookSucceeded;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[EngineShutdown] Removing engine shutdown hooks...");

		// Set the cleanup flag FIRST to prevent hooks from firing during removal
		g_shutdownInProgress = true;

		// CRITICAL: Clear callbacks FIRST to prevent any pending hook invocations
		// from trying to call into already-freed plugin memory
		g_pluginCallbacks.clear();
		ModLoaderLogger::LogDebug(L"[EngineShutdown] Plugin callbacks cleared");

		// Then remove the hooks
		g_engineLoopExitHook.Remove();
		g_enginePreExitHook.Remove();

		ModLoaderLogger::LogDebug(L"[EngineShutdown] All hooks removed");
	}

	void RegisterPluginCallback(PluginEngineShutdownCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineShutdown] RegisterPluginCallback: null callback");
			return;
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[EngineShutdown] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginEngineShutdownCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[EngineShutdown] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
