#include "pch.h"
#include "engine_init.h"
#include "../../logger.h"
#include "../../scanner.h"
#include <vector>
#include <algorithm>

namespace Hooks::EngineInit
{
	static Hook g_hook;
	static FEngineLoop_Init_t g_original = nullptr;
	static bool g_engineInitialized = false;
	static long g_callCount = 0;

	// Callback for plugins to receive engine init events
	static std::vector<PluginEngineInitCallback> g_pluginCallbacks;

	static int32_t __fastcall Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoader::LogInfo(L"[EngineInit] Engine initialization detected (#%ld)", callNum);
		ModLoader::LogDebug(L"[EngineInit]   FEngineLoop=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original
		int32_t result = 0;
		if (g_original)
		{
			ModLoader::LogDebug(L"[EngineInit]   Calling original FEngineLoop::Init...");
			result = g_original(thisPtr);
			ModLoader::LogDebug(L"[EngineInit]   Original returned: %d", result);
		}
		else
		{
			ModLoader::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Mark as initialized and notify all registered plugins (with error isolation)
		if (!g_engineInitialized)
		{
			g_engineInitialized = true;

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

		ModLoader::LogDebug(L"[EngineInit] FEngineLoop::Init complete (#%ld)", callNum);
		return result;
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[EngineInit] Installing hook...");

		// Pattern for FEngineLoop::Init
		const char* pattern = 
			"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? 48 8D 15";

		ModLoader::LogInfo(L"[EngineInit] Scanning for FEngineLoop::Init...");
		ModLoader::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[EngineInit] FEngineLoop::Init not found - mod cannot initialize");
			ModLoader::LogError(L"[EngineInit] This is a CRITICAL error - mod will not function");
			return false;
		} 

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogInfo(L"[EngineInit] ✓ FEngineLoop::Init found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
		{
			ModLoader::LogInfo(L"[EngineInit] Hook installed successfully - waiting for engine to be ready...");
		}
		else
		{
			ModLoader::LogError(L"[EngineInit] Hook installation failed - mod cannot function");
		}

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[EngineInit] Removing hook...");
		g_hook.Remove();
		
		// Clear plugin callbacks
		g_pluginCallbacks.clear();
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
