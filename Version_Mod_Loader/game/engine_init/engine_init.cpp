#include "pch.h"
#include "engine_init.h"
#include "../../logger.h"
#include "../../scanner.h"
#include "../../engine_allocator.h"
#include <vector>
#include <algorithm>

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

		ModLoader::LogInfo(L"[EngineInit] *** ENGINE READY *** (via %s)", hookSource);

		// Resolve the engine allocator (FMemory::Malloc / FMemory::Free) now
		// that the engine is fully initialised.  This must happen before plugin
		// callbacks fire so they can use IPluginHooks::EngineAlloc / EngineFree.
		if (!EngineAllocator::Resolve())
		{
			ModLoader::LogWarn(L"[EngineInit] Engine allocator resolution failed - "
				L"plugins will not be able to use EngineAlloc/EngineFree");
		}

		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[EngineInit] Notifying %zu plugin(s)...", g_pluginCallbacks.size());
			
			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				// Safe invocation of plugin callbacks
				ModLoader::LogTrace(L"[EngineInit]   Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[EngineInit] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[EngineInit] Unknown exception in callback");
				}
			}

			ModLoader::LogDebug(L"[EngineInit] All plugin callbacks completed");
		}
	}

	// Hook 1: FEngineLoop::Init (primary)
	static int32_t __fastcall FEngineLoop_Init_Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoader::LogInfo(L"[EngineInit] FEngineLoop::Init called (#%ld)", callNum);
		ModLoader::LogDebug(L"[EngineInit]   FEngineLoop=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original
		int32_t result = 0;
		if (g_engineLoopOriginal)
		{
			ModLoader::LogDebug(L"[EngineInit]   Calling original FEngineLoop::Init...");
			result = g_engineLoopOriginal(thisPtr);
			ModLoader::LogDebug(L"[EngineInit]   Original returned: %d", result);
		}
		else
		{
			ModLoader::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Notify plugins that engine is ready
		NotifyEngineReady(L"FEngineLoop::Init");

		ModLoader::LogDebug(L"[EngineInit] FEngineLoop::Init complete (#%ld)", callNum);
		return result;
	}

	// Hook 2: UGameEngine::Init (fallback)
	static bool __fastcall UGameEngine_Init_Detour(void* thisPtr, void* InEngineLoop)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoader::LogInfo(L"[EngineInit] UGameEngine::Init called (#%ld)", callNum);
		ModLoader::LogDebug(L"[EngineInit]   GameEngine=%p, EngineLoop=%p, Thread=%lu", 
			thisPtr, InEngineLoop, GetCurrentThreadId());

		// Call original
		bool result = false;
		if (g_gameEngineOriginal)
		{
			ModLoader::LogDebug(L"[EngineInit]   Calling original UGameEngine::Init...");
			result = g_gameEngineOriginal(thisPtr, InEngineLoop);
			ModLoader::LogDebug(L"[EngineInit]   Original returned: %s", result ? L"true" : L"false");
		}
		else
		{
			ModLoader::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Notify plugins that engine is ready (if not already notified)
		NotifyEngineReady(L"UGameEngine::Init");

		ModLoader::LogDebug(L"[EngineInit] UGameEngine::Init complete (#%ld)", callNum);
		return result;
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[EngineInit] Installing engine initialization hooks...");

		bool anyHookSucceeded = false;

		// Try Hook 1: FEngineLoop::Init (primary)
		{
			const char* pattern = 
				"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? 48 8D 15";

			ModLoader::LogInfo(L"[EngineInit] Scanning for FEngineLoop::Init...");
			ModLoader::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

			if (addr)
			{
				HMODULE mainModule = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<uintptr_t>(mainModule);

				ModLoader::LogInfo(L"[EngineInit] [OK] FEngineLoop::Init found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_engineLoopHook.Install(
					addr,
					reinterpret_cast<void*>(&FEngineLoop_Init_Detour),
					reinterpret_cast<void**>(&g_engineLoopOriginal));

				if (hookOk)
				{
					ModLoader::LogInfo(L"[EngineInit] [OK] FEngineLoop::Init hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoader::LogWarn(L"[EngineInit] [FAIL] FEngineLoop::Init hook installation failed");
				}
			}
			else
			{
				ModLoader::LogWarn(L"[EngineInit] [FAIL] FEngineLoop::Init pattern not found - will try fallback");
			}
		}

		// Try Hook 2: UGameEngine::Init (fallback)
		{
			const char* pattern = 
				"48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 45 33 E4 48 89 4D";

			ModLoader::LogInfo(L"[EngineInit] Scanning for UGameEngine::Init (fallback)...");
			ModLoader::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

			if (addr)
			{
				HMODULE mainModule = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<uintptr_t>(mainModule);

				ModLoader::LogInfo(L"[EngineInit] [OK] UGameEngine::Init found at 0x%llX (base+0x%llX)",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - base));

				bool hookOk = g_gameEngineHook.Install(
					addr,
					reinterpret_cast<void*>(&UGameEngine_Init_Detour),
					reinterpret_cast<void**>(&g_gameEngineOriginal));

				if (hookOk)
				{
					ModLoader::LogInfo(L"[EngineInit] [OK] UGameEngine::Init hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoader::LogWarn(L"[EngineInit] [FAIL] UGameEngine::Init hook installation failed");
				}
			}
			else
			{
				ModLoader::LogWarn(L"[EngineInit] [FAIL] UGameEngine::Init pattern not found");
			}
		}

		// Final status
		if (anyHookSucceeded)
		{
			ModLoader::LogInfo(L"[EngineInit] At least one engine init hook installed - engine ready detection active");
		}
		else
		{
			ModLoader::LogError(L"[EngineInit] CRITICAL: No engine init hooks could be installed!");
			ModLoader::LogError(L"[EngineInit] Plugins requiring engine init callbacks will NOT work!");
		}

		return anyHookSucceeded;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[EngineInit] Removing engine init hooks...");
		
		g_engineLoopHook.Remove();
		g_gameEngineHook.Remove();
		
		// Clear plugin callbacks
		g_pluginCallbacks.clear();
		
		ModLoader::LogInfo(L"[EngineInit] All hooks removed");
	}

	bool IsEngineInitialized()
	{
		return g_engineInitialized;
	}

	void RegisterPluginCallback(PluginEngineInitCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[EngineInit] RegisterPluginCallback: null callback provided");
			return;
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[EngineInit] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginEngineInitCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[EngineInit] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}

	void SetEngineInitCallback(void (*callback)())
	{
		// Legacy compatibility - just use RegisterPluginCallback
		if (callback)
		{
			ModLoader::LogWarn(L"[EngineInit] SetEngineInitCallback is deprecated, use RegisterPluginCallback");
			RegisterPluginCallback(callback);
		}
	}
}
