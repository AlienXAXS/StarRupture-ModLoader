#include "pch.h"
#include "save_loaded.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::SaveLoaded
{
	// UCrMassSaveSubsystem::OnSaveLoaded(UCrMassSaveSubsystem* this)
	using OnSaveLoaded_t = void(__fastcall*)(void* thisPtr);

	static Hook g_hook;
	static OnSaveLoaded_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive save-loaded events
	static std::vector<PluginSaveLoadedCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[SaveLoaded]   this=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original first so the save is fully loaded before we notify plugins
		if (g_original)
		{
			ModLoaderLogger::LogDebug(L"[SaveLoaded]   Calling original OnSaveLoaded...");
			g_original(thisPtr);
			ModLoaderLogger::LogDebug(L"[SaveLoaded]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[SaveLoaded] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[SaveLoaded] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[SaveLoaded]Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[SaveLoaded] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[SaveLoaded] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[SaveLoaded] All plugin callbacks completed");
		}

		ModLoaderLogger::LogDebug(L"[SaveLoaded] OnSaveLoaded complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[SaveLoaded] Installing hook...");

		const char* pattern = ScanPatterns::UCrMassSaveSubsystem_OnSaveLoaded;

		ModLoaderLogger::LogInfo(L"[SaveLoaded] Scanning for UCrMassSaveSubsystem::OnSaveLoaded...");
		ModLoaderLogger::LogDebug(L"[SaveLoaded]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule("UCrMassSaveSubsystem::OnSaveLoaded", pattern);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoaderLogger::LogInfo(L"[SaveLoaded] UCrMassSaveSubsystem::OnSaveLoaded found at 0x%llX (base+0x%llX)",
		                         static_cast<unsigned long long>(addr),
		                         static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[SaveLoaded] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[SaveLoaded] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[SaveLoaded] Removing hook...");
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
			ModLoaderLogger::LogWarn(L"[SaveLoaded] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[SaveLoaded] First callback registered — installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[SaveLoaded] Failed to install hook for save-loaded callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[SaveLoaded] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginSaveLoadedCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[SaveLoaded] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}
}
