#include "hooks_interface.h"
#include "hooks_common.h"
#include "logging/logger.h"
#include "hooks/memory/engine_allocator.h"
#include "hooks/game/world_begin_play/world_begin_play.h"
#include "hooks/game/engine_init/engine_init.h"
#include "hooks/game/engine_shutdown/engine_shutdown.h"
#include "hooks/game/save_loaded/save_loaded.h"
#include "hooks/game/experience_load_complete/experience_load_complete.h"
#include "hooks/game/engine_tick/engine_tick.h"
#include "hooks/game/actor_begin_play/actor_begin_play.h"
#include "hooks/game/player_joined/player_joined.h"
#include "hooks/game/player_left/player_left.h"
#include "hooks/game/mass_spawner_activate/mass_spawner_activate.h"
#include "hooks/game/mass_spawner_deactivate/mass_spawner_deactivate.h"
#include "hooks/game/mass_do_spawning/mass_do_spawning.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/input/keybind_registry.h"
#include "hooks/input/input_processor.h"
#include "UI/plugin_panel_registry.h"
#endif
#include <unordered_map>
#include <mutex>

namespace ModLoaderLogger
{
	// Store hook objects by handle
	static std::unordered_map<HookHandle, Hooks::Hook*> g_hookMap;
	static std::mutex g_hookMapMutex;
	static uint64_t g_nextHandleId = 1;

	// Create a unique handle for a hook
	static HookHandle CreateHandle(Hooks::Hook* hook)
	{
		HookHandle handle = reinterpret_cast<HookHandle>(g_nextHandleId++);

		std::lock_guard<std::mutex> lock(g_hookMapMutex);
		g_hookMap[handle] = hook;

		return handle;
	}

	// Get hook from handle
	static Hooks::Hook* GetHook(HookHandle handle)
	{
		if (!handle)
			return nullptr;

		std::lock_guard<std::mutex> lock(g_hookMapMutex);
		auto it = g_hookMap.find(handle);
		return (it != g_hookMap.end()) ? it->second : nullptr;
	}

	// Remove hook from map
	static void RemoveHandle(HookHandle handle)
	{
		if (!handle)
			return;

		std::lock_guard<std::mutex> lock(g_hookMapMutex);
		g_hookMap.erase(handle);
	}

	// Interface implementations
	static HookHandle HooksInstallHook(uintptr_t targetAddress, void* detourFunction, void** originalFunction)
	{
		if (!targetAddress || !detourFunction || !originalFunction)
		{
			LogMessage(L"[HooksInterface] ERROR: Invalid parameters to InstallHook");
			return nullptr;
		}

		// Allocate a new hook object
		Hooks::Hook* hook = new Hooks::Hook();

		// Try to install the hook
		if (!hook->Install(targetAddress, detourFunction, originalFunction))
		{
			LogMessage(L"[HooksInterface] ERROR: Hook installation failed at 0x%llX",
				static_cast<unsigned long long>(targetAddress));
			delete hook;
			return nullptr;
		}

		// Create and return handle
		HookHandle handle = CreateHandle(hook);
		LogMessage(L"[HooksInterface] Hook installed successfully: handle=%p, target=0x%llX",
			handle, static_cast<unsigned long long>(targetAddress));

		return handle;
	}

	static void HooksRemoveHook(HookHandle handle)
	{
		if (!handle)
		{
			LogMessage(L"[HooksInterface] WARN: RemoveHook called with null handle");
			return;
		}

		Hooks::Hook* hook = GetHook(handle);
		if (!hook)
		{
			LogMessage(L"[HooksInterface] ERROR: Invalid hook handle: %p", handle);
			return;
		}

		// Remove the hook
		hook->Remove();

		// Clean up
		RemoveHandle(handle);
		delete hook;

		LogMessage(L"[HooksInterface] Hook removed: handle=%p", handle);
	}

	static bool HooksIsHookInstalled(HookHandle handle)
	{
		if (!handle)
			return false;

		Hooks::Hook* hook = GetHook(handle);
		return hook && hook->installed;
	}

	static bool HooksPatchMemory(uintptr_t address, const uint8_t* data, size_t size)
	{
		return Hooks::Patch(address, data, size);
	}

	static bool HooksNopMemory(uintptr_t address, size_t size)
	{
		return Hooks::Nop(address, size);
	}

	static bool HooksReadMemory(uintptr_t address, void* buffer, size_t size)
	{
		return Hooks::ReadMemory(address, buffer, size);
	}

	static void HooksRegisterWorldBeginPlayCallback(void (*callback)(SDK::UWorld*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterWorldBeginPlayCallback: null callback");
			return;
		}

		Hooks::WorldBeginPlay::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] WorldBeginPlay callback registered for plugin");
	}

	static void HooksUnregisterWorldBeginPlayCallback(void (*callback)(SDK::UWorld*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterWorldBeginPlayCallback: null callback");
			return;
		}

		Hooks::WorldBeginPlay::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] WorldBeginPlay callback unregistered for plugin");
	}

	static void HooksRegisterEngineInitCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterEngineInitCallback: null callback");
			return;
		}

		Hooks::EngineInit::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] EngineInit callback registered for plugin");
	}

	static void HooksUnregisterEngineInitCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterEngineInitCallback: null callback");
			return;
		}

		Hooks::EngineInit::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] EngineInit callback unregistered for plugin");
	}

	static void HooksRegisterEngineShutdownCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterEngineShutdownCallback: null callback");
			return;
		}

		Hooks::EngineShutdown::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] EngineShutdown callback registered for plugin");
	}

	static void HooksUnregisterEngineShutdownCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterEngineShutdownCallback: null callback");
			return;
		}

		Hooks::EngineShutdown::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] EngineShutdown callback unregistered for plugin");
	}

	static void HooksRegisterAnyWorldBeginPlayCallback(void (*callback)(SDK::UWorld*, const char*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterAnyWorldBeginPlayCallback: null callback");
			return;
		}

		Hooks::WorldBeginPlay::RegisterAnyWorldCallback(callback);
		LogDebug(L"[HooksInterface] AnyWorldBeginPlay callback registered for plugin");
	}

	static void HooksUnregisterAnyWorldBeginPlayCallback(void (*callback)(SDK::UWorld*, const char*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterAnyWorldBeginPlayCallback: null callback");
			return;
		}

		Hooks::WorldBeginPlay::UnregisterAnyWorldCallback(callback);
		LogDebug(L"[HooksInterface] AnyWorldBeginPlay callback unregistered for plugin");
	}

	static void HooksRegisterSaveLoadedCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterSaveLoadedCallback: null callback");
			return;
		}

		Hooks::SaveLoaded::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] SaveLoaded callback registered for plugin");
	}

	static void HooksUnregisterSaveLoadedCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterSaveLoadedCallback: null callback");
			return;
		}

		Hooks::SaveLoaded::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] SaveLoaded callback unregistered for plugin");
	}

	static void HooksRegisterExperienceLoadCompleteCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterExperienceLoadCompleteCallback: null callback");
			return;
		}

		Hooks::ExperienceLoadComplete::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] ExperienceLoadComplete callback registered for plugin");
	}

	static void HooksUnregisterExperienceLoadCompleteCallback(void (*callback)())
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterExperienceLoadCompleteCallback: null callback");
			return;
		}

		Hooks::ExperienceLoadComplete::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] ExperienceLoadComplete callback unregistered for plugin");
	}

	// --- Engine allocator wrappers ---

	static void* HooksEngineAlloc(size_t count, uint32_t alignment)
	{
		return EngineAllocator::Alloc(count, alignment);
	}

	static void HooksEngineFree(void* ptr)
	{
		EngineAllocator::Free(ptr);
	}

	static bool HooksIsEngineAllocatorAvailable()
	{
		return EngineAllocator::IsAvailable();
	}

	static void HooksRegisterEngineTickCallback(void (*callback)(float))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterEngineTickCallback: null callback");
			return;
		}

		Hooks::EngineTick::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] EngineTick callback registered for plugin");
	}

	static void HooksUnregisterEngineTickCallback(void (*callback)(float))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterEngineTickCallback: null callback");
			return;
		}

		Hooks::EngineTick::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] EngineTick callback unregistered for plugin");
	}

	static void HooksRegisterActorBeginPlayCallback(void (*callback)(void*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterActorBeginPlayCallback: null callback");
			return;
		}

		Hooks::ActorBeginPlay::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] ActorBeginPlay callback registered for plugin");
	}

	static void HooksUnregisterActorBeginPlayCallback(void (*callback)(void*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterActorBeginPlayCallback: null callback");
			return;
		}

		Hooks::ActorBeginPlay::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] ActorBeginPlay callback unregistered for plugin");
	}

	static void HooksRegisterPlayerJoinedCallback(void (*callback)(void*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterPlayerJoinedCallback: null callback");
			return;
		}

		Hooks::PlayerJoined::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] PlayerJoined callback registered for plugin");
	}

	static void HooksUnregisterPlayerJoinedCallback(void (*callback)(void*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterPlayerJoinedCallback: null callback");
			return;
		}

		Hooks::PlayerJoined::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] PlayerJoined callback unregistered for plugin");
	}

	static void HooksRegisterPlayerLeftCallback(void (*callback)(void*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterPlayerLeftCallback: null callback");
			return;
		}

		Hooks::PlayerLeft::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] PlayerLeft callback registered for plugin");
	}

	static void HooksUnregisterPlayerLeftCallback(void (*callback)(void*))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterPlayerLeftCallback: null callback");
			return;
		}

		Hooks::PlayerLeft::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] PlayerLeft callback unregistered for plugin");
	}

	// --- Spawner sub-interface wrappers (v14) ---

	static void HooksRegisterOnBeforeActivate(PluginBeforeActivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnBeforeActivate: null callback"); return; }
		Hooks::MassSpawnerActivate::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeActivate callback registered");
	}

	static void HooksUnregisterOnBeforeActivate(PluginBeforeActivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] UnregisterOnBeforeActivate: null callback"); return; }
		Hooks::MassSpawnerActivate::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeActivate callback unregistered");
	}

	static void HooksRegisterOnAfterActivate(PluginAfterActivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnAfterActivate: null callback"); return; }
		Hooks::MassSpawnerActivate::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterActivate callback registered");
	}

	static void HooksUnregisterOnAfterActivate(PluginAfterActivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] UnregisterOnAfterActivate: null callback"); return; }
		Hooks::MassSpawnerActivate::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterActivate callback unregistered");
	}

	static void HooksRegisterOnBeforeDeactivate(PluginBeforeDeactivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnBeforeDeactivate: null callback"); return; }
		Hooks::MassSpawnerDeactivate::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDeactivate callback registered");
	}

	static void HooksUnregisterOnBeforeDeactivate(PluginBeforeDeactivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] UnregisterOnBeforeDeactivate: null callback"); return; }
		Hooks::MassSpawnerDeactivate::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDeactivate callback unregistered");
	}

	static void HooksRegisterOnAfterDeactivate(PluginAfterDeactivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnAfterDeactivate: null callback"); return; }
		Hooks::MassSpawnerDeactivate::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDeactivate callback registered");
	}

	static void HooksUnregisterOnAfterDeactivate(PluginAfterDeactivateSpawnerCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] UnregisterOnAfterDeactivate: null callback"); return; }
		Hooks::MassSpawnerDeactivate::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDeactivate callback unregistered");
	}

	static void HooksRegisterOnBeforeDoSpawning(PluginBeforeDoSpawningCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnBeforeDoSpawning: null callback"); return; }
		Hooks::MassDoSpawning::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDoSpawning callback registered");
	}

	static void HooksUnregisterOnBeforeDoSpawning(PluginBeforeDoSpawningCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] UnregisterOnBeforeDoSpawning: null callback"); return; }
		Hooks::MassDoSpawning::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDoSpawning callback unregistered");
	}

	static void HooksRegisterOnAfterDoSpawning(PluginAfterDoSpawningCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnAfterDoSpawning: null callback"); return; }
		Hooks::MassDoSpawning::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDoSpawning callback registered");
	}

	static void HooksUnregisterOnAfterDoSpawning(PluginAfterDoSpawningCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] UnregisterOnAfterDoSpawning: null callback"); return; }
		Hooks::MassDoSpawning::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDoSpawning callback unregistered");
	}

	// Spawner sub-interface struct (v14)
	static IPluginSpawnerHooks g_spawnerHooks = {
		HooksRegisterOnBeforeActivate,
		HooksUnregisterOnBeforeActivate,
		HooksRegisterOnAfterActivate,
		HooksUnregisterOnAfterActivate,
		HooksRegisterOnBeforeDeactivate,
		HooksUnregisterOnBeforeDeactivate,
		HooksRegisterOnAfterDeactivate,
		HooksUnregisterOnAfterDeactivate,
		HooksRegisterOnBeforeDoSpawning,
		HooksUnregisterOnBeforeDoSpawning,
		HooksRegisterOnAfterDoSpawning,
		HooksUnregisterOnAfterDoSpawning
	};

	// Utility sub-interface structs (v14) — populated from existing wrapper functions
	static IPluginHookUtils g_hookUtils = {
		HooksInstallHook,
		HooksRemoveHook,
		HooksIsHookInstalled
	};

	static IPluginMemoryUtils g_memoryUtils = {
		HooksPatchMemory,
		HooksNopMemory,
		HooksReadMemory,
		HooksEngineAlloc,
		HooksEngineFree,
		HooksIsEngineAllocatorAvailable
	};

	static IPluginEngineEvents g_engineEvents = {
		HooksRegisterEngineInitCallback,
		HooksUnregisterEngineInitCallback,
		HooksRegisterEngineShutdownCallback,
		HooksUnregisterEngineShutdownCallback,
		HooksRegisterEngineTickCallback,
		HooksUnregisterEngineTickCallback
	};

	static IPluginWorldEvents g_worldEvents = {
		HooksRegisterWorldBeginPlayCallback,
		HooksUnregisterWorldBeginPlayCallback,
		HooksRegisterAnyWorldBeginPlayCallback,
		HooksUnregisterAnyWorldBeginPlayCallback,
		HooksRegisterSaveLoadedCallback,
		HooksUnregisterSaveLoadedCallback,
		HooksRegisterExperienceLoadCompleteCallback,
		HooksUnregisterExperienceLoadCompleteCallback
	};

	static IPluginPlayerEvents g_playerEvents = {
		HooksRegisterPlayerJoinedCallback,
		HooksUnregisterPlayerJoinedCallback,
		HooksRegisterPlayerLeftCallback,
		HooksUnregisterPlayerLeftCallback
	};

	static IPluginActorEvents g_actorEvents = {
		HooksRegisterActorBeginPlayCallback,
		HooksUnregisterActorBeginPlayCallback
	};

	// --- Input sub-interface wrappers (v15, client only) ---

#ifdef MODLOADER_CLIENT_BUILD
	static void HooksRegisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterKeybind: null callback"); return; }
		Hooks::Input::RegisterKeybind(key, event, callback);
		LogDebug(L"[HooksInterface] Keybind registered (enum key=%u, event=%u)",
			static_cast<unsigned>(key), static_cast<unsigned>(event));
	}

	static void HooksUnregisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback) return;
		Hooks::Input::UnregisterKeybind(key, event, callback);
		LogDebug(L"[HooksInterface] Keybind unregistered (enum key=%u, event=%u)",
			static_cast<unsigned>(key), static_cast<unsigned>(event));
	}

	static void HooksRegisterKeybindByName(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback || !keyName) { LogWarn(L"[HooksInterface] RegisterKeybindByName: null argument"); return; }
		Hooks::Input::RegisterKeybindByName(keyName, event, callback);
		LogDebug(L"[HooksInterface] Keybind registered (name=%S, event=%u)",
			keyName, static_cast<unsigned>(event));
	}

	static void HooksUnregisterKeybindByName(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback || !keyName) return;
		Hooks::Input::UnregisterKeybindByName(keyName, event, callback);
		LogDebug(L"[HooksInterface] Keybind unregistered (name=%S, event=%u)",
			keyName, static_cast<unsigned>(event));
	}

	// Input sub-interface struct (v15)
	static IPluginInputEvents g_inputEvents = {
		HooksRegisterKeybind,
		HooksUnregisterKeybind,
		HooksRegisterKeybindByName,
		HooksUnregisterKeybindByName
	};

	// --- UI sub-interface wrappers (v15, client only) ---

	static PanelHandle HooksRegisterPanel(const PluginPanelDesc* desc)
	{
		return UI::PluginPanelRegistry::RegisterPanel(desc);
	}

	static void HooksUnregisterPanel(PanelHandle handle)
	{
		UI::PluginPanelRegistry::UnregisterPanel(handle);
	}

	static void HooksRegisterOnConfigChanged(PluginConfigChangedCallback callback)
	{
		if (!callback) { LogWarn(L"[HooksInterface] RegisterOnConfigChanged: null callback"); return; }
		UI::PluginPanelRegistry::RegisterOnConfigChanged(callback);
	}

	static void HooksUnregisterOnConfigChanged(PluginConfigChangedCallback callback)
	{
		if (!callback) return;
		UI::PluginPanelRegistry::UnregisterOnConfigChanged(callback);
	}

	static void HooksSetPanelOpen(PanelHandle handle)
	{
		UI::PluginPanelRegistry::SetPanelOpen(handle);
	}

	static void HooksSetPanelClose(PanelHandle handle)
	{
		UI::PluginPanelRegistry::SetPanelClose(handle);
	}

	// UI sub-interface struct (v15)
	static IPluginUIEvents g_uiEvents = {
		HooksRegisterPanel,
		HooksUnregisterPanel,
		HooksRegisterOnConfigChanged,
		HooksUnregisterOnConfigChanged,
		HooksSetPanelOpen,
		HooksSetPanelClose
	};
#endif // MODLOADER_CLIENT_BUILD

	// Global hooks interface instance
	static IPluginHooks g_pluginHooks = {
		&g_spawnerHooks,
		&g_hookUtils,
		&g_memoryUtils,
		&g_engineEvents,
		&g_worldEvents,
		&g_playerEvents,
		&g_actorEvents,
#ifdef MODLOADER_CLIENT_BUILD
		&g_inputEvents,   // v15 — keybind events (client only)
		&g_uiEvents       // v15 — custom panel + config-change callbacks (client only)
#else
		nullptr,          // v15 — Input is null on server/generic builds
		nullptr           // v15 — UI is null on server/generic builds
#endif
	};

	IPluginHooks* GetPluginHooks()
	{
		return &g_pluginHooks;
	}
}
