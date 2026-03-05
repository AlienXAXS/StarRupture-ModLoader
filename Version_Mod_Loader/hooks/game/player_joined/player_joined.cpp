#include "pch.h"
#include "player_joined.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::PlayerJoined
{
	// ACrGameModeBase::PostLogin(ACrGameModeBase* this, APlayerController* NewPlayer)
	typedef void(__fastcall* PostLogin_t)(void* thisPtr, void* newPlayer);

	static Hook g_hook;
	static PostLogin_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive player-joined events
	static std::vector<PluginPlayerJoinedCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr, void* newPlayer)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[PlayerJoined] ACrGameModeBase::PostLogin called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[PlayerJoined]   this=%p, NewPlayer=%p, Thread=%lu",
			thisPtr, newPlayer, GetCurrentThreadId());

		// Call original first so the player is fully set up before we notify plugins
		if (g_original)
		{
			ModLoaderLogger::LogDebug(L"[PlayerJoined]   Calling original PostLogin...");
			g_original(thisPtr, newPlayer);
			ModLoaderLogger::LogDebug(L"[PlayerJoined]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[PlayerJoined] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[PlayerJoined] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoaderLogger::LogTrace(L"[PlayerJoined] Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i](newPlayer);
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[PlayerJoined] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[PlayerJoined] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[PlayerJoined] All plugin callbacks completed");
		}

		ModLoaderLogger::LogDebug(L"[PlayerJoined] PlayerJoined complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[PlayerJoined] Installing hook...");

		const char* pattern = ScanPatterns::ACrGameModeBase_PostLogin;

		ModLoaderLogger::LogInfo(L"[PlayerJoined] Scanning for ACrGameModeBase::PostLogin...");
		ModLoaderLogger::LogDebug(L"[PlayerJoined]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[PlayerJoined] ACrGameModeBase::PostLogin pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoaderLogger::LogInfo(L"[PlayerJoined] ACrGameModeBase::PostLogin found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[PlayerJoined] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[PlayerJoined] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[PlayerJoined] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginPlayerJoinedCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[PlayerJoined] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[PlayerJoined] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[PlayerJoined] Failed to install hook for player-joined callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[PlayerJoined] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginPlayerJoinedCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[PlayerJoined] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
