#include "pch.h"
#include "save_loaded.h"
#include "../../logger.h"
#include "../../scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::SaveLoaded
{
	// UCrMassSaveSubsystem::OnSaveLoaded(UCrMassSaveSubsystem* this)
	typedef void(__fastcall* OnSaveLoaded_t)(void* thisPtr);

	static Hook g_hook;
	static OnSaveLoaded_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive save-loaded events
	static std::vector<PluginSaveLoadedCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoader::LogInfo(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded called (#%ld)", callNum);
		ModLoader::LogDebug(L"[SaveLoaded]   this=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original first so the save is fully loaded before we notify plugins
		if (g_original)
		{
			ModLoader::LogDebug(L"[SaveLoaded]   Calling original OnSaveLoaded...");
			g_original(thisPtr);
			ModLoader::LogDebug(L"[SaveLoaded]   Original returned");
		}
		else
		{
			ModLoader::LogError(L"[SaveLoaded] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[SaveLoaded] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoader::LogTrace(L"[SaveLoaded]Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[SaveLoaded] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[SaveLoaded] Unknown exception in callback");
				}
			}

			ModLoader::LogDebug(L"[SaveLoaded] All plugin callbacks completed");
		}

		ModLoader::LogDebug(L"[SaveLoaded] OnSaveLoaded complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[SaveLoaded] Installing hook...");

		const char* pattern = ScanPatterns::UCrMassSaveSubsystem_OnSaveLoaded;

		ModLoader::LogInfo(L"[SaveLoaded] Scanning for UCrMassSaveSubsystem::OnSaveLoaded...");
		ModLoader::LogDebug(L"[SaveLoaded]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogInfo(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[SaveLoaded] Hook installed successfully");
		else
			ModLoader::LogError(L"[SaveLoaded] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[SaveLoaded] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginSaveLoadedCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[SaveLoaded] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[SaveLoaded] First callback registered — installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[SaveLoaded] Failed to install hook for save-loaded callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[SaveLoaded] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginSaveLoadedCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[SaveLoaded] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
