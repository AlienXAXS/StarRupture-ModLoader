#include "pch.h"
#include "engine_tick.h"
#include "../../logger.h"
#include "../../scanner.h"
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
		ModLoader::LogInfo(L"[EngineTick] Installing UGameEngine::Tick hook...");

		const char* pattern = ScanPatterns::UGameEngine_Tick;

		ModLoader::LogDebug(L"[EngineTick]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[EngineTick] UGameEngine::Tick pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogInfo(L"[EngineTick] UGameEngine::Tick found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[EngineTick] Hook installed successfully");
		else
			ModLoader::LogError(L"[EngineTick] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[EngineTick] Removing hook...");
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
			ModLoader::LogWarn(L"[EngineTick] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[EngineTick] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[EngineTick] Failed to install hook for engine tick callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[EngineTick] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginEngineTickCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[EngineTick] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
