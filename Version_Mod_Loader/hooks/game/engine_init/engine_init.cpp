#include "pch.h"
#include "engine_init.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "hooks/memory/engine_allocator.h"
#include <vector>
#include <algorithm>
#include "../scan_patterns.h"

namespace Hooks::EngineInit
{
	// Hook objects for multiple initialization points
	static Hook g_engineLoopHook;
	static Hook g_gameEngineHook;
	
	// Original function pointers
	static FEngineLoop_Init_t g_engineLoopOriginal = nullptr;
	static UGameEngine_Init_t g_gameEngineOriginal = nullptr;
	
	static bool g_engineInitialized = false;
	static long g_callCount = 0;

	// Callback for plugins to receive engine init events
	static std::vector<PluginEngineInitCallback> g_pluginCallbacks;

	// Shared notification function - called by any successful hook
	static void NotifyEngineReady(const wchar_t* hookSource)
	{
		// Only notify once, regardless of which hook fires first
		if (g_engineInitialized)
		{
			return;
		}

		g_engineInitialized = true;

		ModLoaderLogger::LogInfo(L"[EngineInit] *** ENGINE READY *** (via %s)", hookSource);

		// Resolve the engine allocator (FMemory::Malloc / FMemory::Free) now
		// that the engine is fully initialised.  This must happen before plugin
		// callbacks fire so they can use IPluginHooks::EngineAlloc / EngineFree.
		if (!EngineAllocator::Resolve())
		{
			ModLoaderLogger::LogWarn(L"[EngineInit] Engine allocator resolution failed - "
				L"plugins will not be able to use EngineAlloc/EngineFree");
		}

		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[EngineInit] Notifying %zu plugin(s)...", g_pluginCallbacks.size());
			
			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				// Safe invocation of plugin callbacks
				ModLoaderLogger::LogTrace(L"[EngineInit]   Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[EngineInit] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[EngineInit] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[EngineInit] All plugin callbacks completed");
		}
	}

	// Hook 1: FEngineLoop::Init (primary)
	static int32_t __fastcall FEngineLoop_Init_Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[EngineInit] FEngineLoop::Init called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[EngineInit]   FEngineLoop=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original
		int32_t result = 0;
		if (g_engineLoopOriginal)
		{
			ModLoaderLogger::LogDebug(L"[EngineInit]   Calling original FEngineLoop::Init...");
			result = g_engineLoopOriginal(thisPtr);
			ModLoaderLogger::LogDebug(L"[EngineInit]   Original returned: %d", result);
		}
		else
		{
			ModLoaderLogger::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Notify plugins that engine is ready
		NotifyEngineReady(L"FEngineLoop::Init");

		ModLoaderLogger::LogDebug(L"[EngineInit] FEngineLoop::Init complete (#%ld)", callNum);
		return result;
	}

	// Hook 2: UGameEngine::Init (fallback)
	static bool __fastcall UGameEngine_Init_Detour(void* thisPtr, void* InEngineLoop)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[EngineInit] UGameEngine::Init called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[EngineInit]   GameEngine=%p, EngineLoop=%p, Thread=%lu", 
			thisPtr, InEngineLoop, GetCurrentThreadId());

		// Call original
		bool result = false;
		if (g_gameEngineOriginal)
		{
			ModLoaderLogger::LogDebug(L"[EngineInit]   Calling original UGameEngine::Init...");
			result = g_gameEngineOriginal(thisPtr, InEngineLoop);
			ModLoaderLogger::LogDebug(L"[EngineInit]   Original returned: %s", result ? L"true" : L"false");
		}
		else
		{
			ModLoaderLogger::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Notify plugins that engine is ready (if not already notified)
		NotifyEngineReady(L"UGameEngine::Init");

		ModLoaderLogger::LogDebug(L"[EngineInit] UGameEngine::Init complete (#%ld)", callNum);
		return result;
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[EngineInit] Installing engine initialization hooks...");

		bool anyHookSucceeded = false;

		// Try Hook 1: FEngineLoop::Init (primary)
		{
			const char* pattern = ScanPatterns::FEngineLoop_Init;
				

			ModLoaderLogger::LogInfo(L"[EngineInit] Scanning for FEngineLoop::Init...");
			ModLoaderLogger::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule("FEngineLoop::Init", pattern);

			if (addr)
			{
				HMODULE mainModule = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<uintptr_t>(mainModule);

				ModLoaderLogger::LogDebug(L"[EngineInit] [OK] FEngineLoop::Init found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_engineLoopHook.Install(
					addr,
					reinterpret_cast<void*>(&FEngineLoop_Init_Detour),
					reinterpret_cast<void**>(&g_engineLoopOriginal));

				if (hookOk)
				{
					ModLoaderLogger::LogInfo(L"[EngineInit] [OK] FEngineLoop::Init hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] FEngineLoop::Init hook installation failed");
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] FEngineLoop::Init pattern not found - will try fallback");
			}
		}

		// Try Hook 2: UGameEngine::Init (fallback)
		{
			const char* pattern = ScanPatterns::UGameEngine_Init;
				

			ModLoaderLogger::LogInfo(L"[EngineInit] Scanning for UGameEngine::Init (fallback)...");
			ModLoaderLogger::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule("UGameEngine::Init", pattern);

			if (addr)
			{
				HMODULE mainModule = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<uintptr_t>(mainModule);

				ModLoaderLogger::LogDebug(L"[EngineInit] [OK] UGameEngine::Init found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_gameEngineHook.Install(
					addr,
					reinterpret_cast<void*>(&UGameEngine_Init_Detour),
					reinterpret_cast<void**>(&g_gameEngineOriginal));

				if (hookOk)
				{
					ModLoaderLogger::LogInfo(L"[EngineInit] [OK] UGameEngine::Init hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] UGameEngine::Init hook installation failed");
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] UGameEngine::Init pattern not found");
			}
		}

		// Final status
		if (anyHookSucceeded)
		{
			ModLoaderLogger::LogInfo(L"[EngineInit] At least one engine init hook installed - engine ready detection active");
		}
		else
		{
			ModLoaderLogger::LogError(L"[EngineInit] CRITICAL: No engine init hooks could be installed!");
			ModLoaderLogger::LogError(L"[EngineInit] Plugins requiring engine init callbacks will NOT work!");
		}

		return anyHookSucceeded;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[EngineInit] Removing engine init hooks...");
		
		g_engineLoopHook.Remove();
		g_gameEngineHook.Remove();
		
		// Clear plugin callbacks
		g_pluginCallbacks.clear();
		
		ModLoaderLogger::LogInfo(L"[EngineInit] All hooks removed");
	}

	bool IsEngineInitialized()
	{
		return g_engineInitialized;
	}

	void RegisterPluginCallback(PluginEngineInitCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineInit] RegisterPluginCallback: null callback provided");
			return;
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[EngineInit] Plugin callback registered (%zu total)", g_pluginCallbacks.size());

		// If the engine is already initialized, invoke the callback immediately
		// so that late-registering plugins (loaded after the hook fires) still
		// receive the notification.
		if (g_engineInitialized)
		{
			ModLoaderLogger::LogDebug(L"[EngineInit] Engine already initialized - invoking callback immediately");
			try
			{
				callback();
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[EngineInit] Exception in late callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[EngineInit] Unknown exception in late callback");
			}
		}
	}

	void UnregisterPluginCallback(PluginEngineInitCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[EngineInit] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}

	void SetEngineInitCallback(void (*callback)())
	{
		// Legacy compatibility - just use RegisterPluginCallback
		if (callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineInit] SetEngineInitCallback is deprecated, use RegisterPluginCallback");
			RegisterPluginCallback(callback);
		}
	}
}
