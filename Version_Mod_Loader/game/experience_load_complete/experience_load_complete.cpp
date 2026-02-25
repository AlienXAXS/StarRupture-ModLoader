#include "pch.h"
#include "experience_load_complete.h"
#include "../../logger.h"
#include "../../scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::ExperienceLoadComplete
{
	// UCrExperienceManagerComponent::OnExperienceLoadComplete(UCrExperienceManagerComponent* this)
	typedef void(__fastcall* OnExperienceLoadComplete_t)(void* thisPtr);

	static Hook g_hook;
	static OnExperienceLoadComplete_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive experience-load-complete events
	static std::vector<PluginExperienceLoadCompleteCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoader::LogInfo(L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete called (#%ld)", callNum);
		ModLoader::LogDebug(L"[ExperienceLoadComplete]   this=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original first so the experience is fully loaded before we notify plugins
		if (g_original)
		{
			ModLoader::LogDebug(L"[ExperienceLoadComplete]   Calling original OnExperienceLoadComplete...");
			g_original(thisPtr);
			ModLoader::LogDebug(L"[ExperienceLoadComplete]   Original returned");
		}
		else
		{
			ModLoader::LogError(L"[ExperienceLoadComplete] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[ExperienceLoadComplete] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoader::LogTrace(L"[ExperienceLoadComplete] Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[ExperienceLoadComplete] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[ExperienceLoadComplete] Unknown exception in callback");
				}
			}

			ModLoader::LogDebug(L"[ExperienceLoadComplete] All plugin callbacks completed");
		}

		ModLoader::LogDebug(L"[ExperienceLoadComplete] OnExperienceLoadComplete complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[ExperienceLoadComplete] Installing hook...");

		const char* pattern = ScanPatterns::UCrExperienceManagerComponent_OnExperienceLoadComplete;

		ModLoader::LogInfo(L"[ExperienceLoadComplete] Scanning for UCrExperienceManagerComponent::OnExperienceLoadComplete...");
		ModLoader::LogDebug(L"[ExperienceLoadComplete]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogInfo(L"[ExperienceLoadComplete] UCrExperienceManagerComponent::OnExperienceLoadComplete found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[ExperienceLoadComplete] Hook installed successfully");
		else
			ModLoader::LogError(L"[ExperienceLoadComplete] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[ExperienceLoadComplete] Removing hook...");
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
			ModLoader::LogWarn(L"[ExperienceLoadComplete] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[ExperienceLoadComplete] First callback registered — installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[ExperienceLoadComplete] Failed to install hook for experience-load-complete callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[ExperienceLoadComplete] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginExperienceLoadCompleteCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[ExperienceLoadComplete] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
