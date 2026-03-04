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

		ModLoader::LogInfo(L"[PlayerJoined] ACrGameModeBase::PostLogin called (#%ld)", callNum);
		ModLoader::LogDebug(L"[PlayerJoined]   this=%p, NewPlayer=%p, Thread=%lu",
			thisPtr, newPlayer, GetCurrentThreadId());

		// Call original first so the player is fully set up before we notify plugins
		if (g_original)
		{
			ModLoader::LogDebug(L"[PlayerJoined]   Calling original PostLogin...");
			g_original(thisPtr, newPlayer);
			ModLoader::LogDebug(L"[PlayerJoined]   Original returned");
		}
		else
		{
			ModLoader::LogError(L"[PlayerJoined] Original function pointer is null!");
		}

		// Notify registered plugins
		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[PlayerJoined] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				ModLoader::LogTrace(L"[PlayerJoined] Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i](newPlayer);
				}
				catch (const std::exception& e)
				{
					ModLoader::LogError(L"[PlayerJoined] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoader::LogError(L"[PlayerJoined] Unknown exception in callback");
				}
			}

			ModLoader::LogDebug(L"[PlayerJoined] All plugin callbacks completed");
		}

		ModLoader::LogDebug(L"[PlayerJoined] PlayerJoined complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[PlayerJoined] Installing hook...");

		const char* pattern = ScanPatterns::ACrGameModeBase_PostLogin;

		ModLoader::LogInfo(L"[PlayerJoined] Scanning for ACrGameModeBase::PostLogin...");
		ModLoader::LogDebug(L"[PlayerJoined]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[PlayerJoined] ACrGameModeBase::PostLogin pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogInfo(L"[PlayerJoined] ACrGameModeBase::PostLogin found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[PlayerJoined] Hook installed successfully");
		else
			ModLoader::LogError(L"[PlayerJoined] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[PlayerJoined] Removing hook...");
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
			ModLoader::LogWarn(L"[PlayerJoined] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[PlayerJoined] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[PlayerJoined] Failed to install hook for player-joined callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[PlayerJoined] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginPlayerJoinedCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[PlayerJoined] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
