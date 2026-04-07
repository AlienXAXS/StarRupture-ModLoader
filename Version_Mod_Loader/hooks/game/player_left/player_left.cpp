#include "pch.h"
#include "player_left.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../ufunction_resolve.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::PlayerLeft
{
	// ACrGameModeBase::Logout(ACrGameModeBase* this, AController* Exiting)
	using Logout_t = void(__fastcall*)(void* thisPtr, void* exiting);

	static Hook g_hook;
	static Logout_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive player-left events
	static std::vector<PluginPlayerLeftCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr, void* exiting)
	{
		void* const callerAddr = _ReturnAddress();

		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[PlayerLeft] ACrGameModeBase::Logout called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[PlayerLeft]   this=%p, Exiting=%p, Thread=%lu",
		                          thisPtr, exiting, GetCurrentThreadId());
		ModLoaderLogger::LogTrace(L"[PlayerLeft]   Called from: %S",
		                          Hooks::GetCallerModuleName(callerAddr).c_str());

		// Notify plugins BEFORE calling original so the controller is still valid
		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[PlayerLeft] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[PlayerLeft] Calling plugin callback #%zu (%S)",
				                          i + 1, Hooks::GetCallerModuleName((void*)g_pluginCallbacks[i]).c_str());

				try
				{
					g_pluginCallbacks[i](exiting);
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[PlayerLeft] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[PlayerLeft] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[PlayerLeft] All plugin callbacks completed");
		}

		// Call original AFTER notifying plugins � the original tears down the controller
		if (g_original)
		{
			ModLoaderLogger::LogDebug(L"[PlayerLeft]   Calling original Logout...");
			g_original(thisPtr, exiting);
			ModLoaderLogger::LogDebug(L"[PlayerLeft]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[PlayerLeft] Original function pointer is null!");
		}

		ModLoaderLogger::LogDebug(L"[PlayerLeft] PlayerLeft complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[PlayerLeft] Installing hook...");

		uintptr_t addr = Hooks::ResolveUFunctionNativeAddr("CrGameModeBase", "Logout");
		if (!addr)
		{
			ModLoaderLogger::LogDebug(L"[PlayerLeft] Not a UFUNCTION -- falling back to pattern scan");
			addr = Scanner::FindPatternInMainModule(
				"ACrGameModeBase::Logout", ScanPatterns::ACrGameModeBase_Logout);
		}
		if (!addr)
			return false;

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[PlayerLeft] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[PlayerLeft] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[PlayerLeft] Removing hook...");
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
			ModLoaderLogger::LogWarn(L"[PlayerLeft] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[PlayerLeft] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[PlayerLeft] Failed to install hook for player-left callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[PlayerLeft] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginPlayerLeftCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[PlayerLeft] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}
}
