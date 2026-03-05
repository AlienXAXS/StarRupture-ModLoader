#include "pch.h"
#include "engine_tick.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::EngineTick
{
	// UGameEngine::Tick(UGameEngine* this, float DeltaSeconds, bool bIdleMode)
	typedef void(__fastcall* UGameEngine_Tick_t)(void* thisPtr, float deltaSeconds, bool bIdleMode);

	static Hook g_hook;
	static UGameEngine_Tick_t g_original = nullptr;

	// Registered plugin callbacks
	static std::vector<PluginEngineTickCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr, float deltaSeconds, bool bIdleMode)
	{
		// Call original first so the engine tick completes before we notify plugins
		if (g_original)
			g_original(thisPtr, deltaSeconds, bIdleMode);

		// Notify registered plugins — keep this path fast (no logging per-frame)
		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (g_pluginCallbacks[i])
			{
				try
				{
					g_pluginCallbacks[i](deltaSeconds);
				}
				catch (...)
				{
					// Swallow exceptions to avoid crashing the main loop.
					// Log only once per callback to avoid spam.
				}
			}
		}
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[EngineTick] Installing UGameEngine::Tick hook...");

		const char* pattern = ScanPatterns::UGameEngine_Tick;

		ModLoaderLogger::LogDebug(L"[EngineTick]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[EngineTick] UGameEngine::Tick pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoaderLogger::LogInfo(L"[EngineTick] UGameEngine::Tick found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[EngineTick] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[EngineTick] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[EngineTick] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginEngineTickCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineTick] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[EngineTick] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[EngineTick] Failed to install hook for engine tick callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[EngineTick] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginEngineTickCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[EngineTick] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
