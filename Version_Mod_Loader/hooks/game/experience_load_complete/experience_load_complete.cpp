#include "pch.h"
#include "experience_load_complete.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::ExperienceLoadComplete
{
	// UCrExperienceManagerComponent::OnExperienceLoadComplete(UCrExperienceManagerComponent* this)
	using OnExperienceLoadComplete_t = void(__fastcall*)(void* thisPtr);

	static Hook g_hook;
	static OnExperienceLoadComplete_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive experience-load-complete events
	static std::vector<PluginExperienceLoadCompleteCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(
			L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   this=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original first so the experience is fully loaded before we notify plugins
		if (g_original)
		{
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   Calling original OnExperienceLoadComplete...");
			g_original(thisPtr);
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[ExperienceLoadComplete] Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] All plugin callbacks completed");
		}

		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] OnExperienceLoadComplete complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] Installing hook...");

		const char* pattern = ScanPatterns::UCrExperienceManagerComponent_OnExperienceLoadComplete;

		ModLoaderLogger::LogInfo(
			L"[ExperienceLoadComplete] Scanning for UCrExperienceManagerComponent::OnExperienceLoadComplete...");
		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule("UCrExperienceManagerComponent::OnExperienceLoadComplete",
		                                                  pattern);

		if (!addr)
		{
			ModLoaderLogger::LogError(
				L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoaderLogger::LogInfo(
			L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[ExperienceLoadComplete] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginExperienceLoadCompleteCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[ExperienceLoadComplete] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[ExperienceLoadComplete] First callback registered — installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(
					L"[ExperienceLoadComplete] Failed to install hook for experience-load-complete callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Plugin callback registered (%zu total)",
		                          g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginExperienceLoadCompleteCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[ExperienceLoadComplete] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}
}
