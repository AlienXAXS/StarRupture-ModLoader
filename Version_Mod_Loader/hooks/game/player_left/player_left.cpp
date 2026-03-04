#include "pch.h"
#include "player_left.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::PlayerLeft
{
	// ACrGameModeBase::Logout(ACrGameModeBase* this, AController* Exiting)
	typedef void(__fastcall* Logout_t)(void* thisPtr, void* exiting);

	static Hook g_hook;
	static Logout_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive player-left events
	static std::vector<PluginPlayerLeftCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr, void* exiting)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoader::LogInfo(L"[PlayerLeft] ACrGameModeBase::Logout called (#%ld)", callNum);
		ModLoader::LogDebug(L"[PlayerLeft]   this=%p, Exiting=%p, Thread=%lu",
			thisPtr, exiting, GetCurrentThreadId());

		// Notify plugins BEFORE calling original so the controller is still valid
		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[PlayerLeft] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoader::LogTrace(L"[PlayerLeft] Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i](exiting);
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[PlayerLeft] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[PlayerLeft] Unknown exception in callback");
				}
			}

			ModLoader::LogDebug(L"[PlayerLeft] All plugin callbacks completed");
		}

		// Call original AFTER notifying plugins — the original tears down the controller
		if (g_original)
		{
			ModLoader::LogDebug(L"[PlayerLeft]   Calling original Logout...");
			g_original(thisPtr, exiting);
			ModLoader::LogDebug(L"[PlayerLeft]   Original returned");
		}
		else
		{
			ModLoader::LogError(L"[PlayerLeft] Original function pointer is null!");
		}

		ModLoader::LogDebug(L"[PlayerLeft] PlayerLeft complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[PlayerLeft] Installing hook...");

		const char* pattern = ScanPatterns::ACrGameModeBase_Logout;

		ModLoader::LogInfo(L"[PlayerLeft] Scanning for ACrGameModeBase::Logout...");
		ModLoader::LogDebug(L"[PlayerLeft]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[PlayerLeft] ACrGameModeBase::Logout pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogInfo(L"[PlayerLeft] ACrGameModeBase::Logout found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[PlayerLeft] Hook installed successfully");
		else
			ModLoader::LogError(L"[PlayerLeft] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[PlayerLeft] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginPlayerLeftCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[PlayerLeft] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[PlayerLeft] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[PlayerLeft] Failed to install hook for player-left callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[PlayerLeft] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginPlayerLeftCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[PlayerLeft] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
