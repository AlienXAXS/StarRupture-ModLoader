#include "pch.h"
#include "actor_begin_play.h"
#include "logging/logger.h"
#include "../ufunction_resolve.h"
#include <vector>
#include <algorithm>

namespace Hooks::ActorBeginPlay
{
	// AActor::BeginPlay(AActor* this)
	using AActor_BeginPlay_t = void(__fastcall*)(void* thisPtr);

	static Hook g_hook;
	static AActor_BeginPlay_t g_original = nullptr;

	// Callback for plugins to receive actor-begin-play events
	static std::vector<PluginActorBeginPlayCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr)
	{
		// Call original first so the actor is fully initialised before we notify
		if (g_original)
		{
			g_original(thisPtr);
		}

		// Notify registered plugins
		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (!g_pluginCallbacks[i])
				continue;

			try
			{
				g_pluginCallbacks[i](thisPtr);
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[ActorBeginPlay] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[ActorBeginPlay] Unknown exception in callback");
			}
		}
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[ActorBeginPlay] Installing hook...");

		uintptr_t addr = Hooks::ResolveUFunctionNativeAddr("Actor", "ReceiveBeginPlay");
		if (!addr)
			addr = Hooks::ResolveUFunctionNativeAddr("Actor", "BeginPlay");
		if (!addr)
			return false;

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[ActorBeginPlay] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[ActorBeginPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[ActorBeginPlay] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginActorBeginPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[ActorBeginPlay] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[ActorBeginPlay] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[ActorBeginPlay] Failed to install hook for actor-begin-play callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[ActorBeginPlay] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginActorBeginPlayCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[ActorBeginPlay] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
