#include "hooks_interface.h"
#include "hooks_common.h"
#include "logging/logger.h"
#include "network_channel/network_channel.h"
#include "hooks/memory/engine_allocator.h"
#include "utils/game_thread_dispatch.h"
#include "hooks/game/world_begin_play/world_begin_play.h"
#include "hooks/game/world_end_play/world_end_play.h"
#include "hooks/game/engine_init/engine_init.h"
#include "hooks/game/engine_shutdown/engine_shutdown.h"
#include "hooks/game/save_loaded/save_loaded.h"
#include "hooks/game/text_localization/text_localization.h"
#include "hooks/game/text_localization/text_key.h"
#include "hooks/game/experience_load_complete/experience_load_complete.h"
#include "hooks/game/engine_tick/engine_tick.h"
#include "hooks/game/actor_begin_play/actor_begin_play.h"
#include "hooks/game/crafting_finished/crafting_finished.h"
#include "hooks/game/player_joined/player_joined.h"
#include "hooks/game/player_left/player_left.h"
#include "hooks/game/mass_spawner_activate/mass_spawner_activate.h"
#include "hooks/game/mass_spawner_deactivate/mass_spawner_deactivate.h"
#include "hooks/game/mass_do_spawning/mass_do_spawning.h"
#include "memory_scanner/scanner.h"
#include "hooks/game/scan_patterns.h"
#include "hooks/game/ufunction_resolve.h"
#ifdef MODLOADER_SERVER_BUILD
#include "hooks/http/http_server_hook.h"
#endif
#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/input/keybind_registry.h"
#include "hooks/input/input_processor.h"
#include "UI/plugin_panel_registry.h"
#include "UI/plugin_widget_registry.h"
#include "UI/imgui_backend.h"
#include "UI/splash_window.h"
#include "hooks/game/hud_post_render/hud_post_render.h"
#include "hooks/game/client_message/client_message.h"
#endif
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#include "hooks/game/session_info/session_info.h"
#endif
#include "hooks/game/object_lookup/object_lookup.h"
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
		auto handle = reinterpret_cast<HookHandle>(g_nextHandleId++);

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
		auto hook = new Hooks::Hook();

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

	// v16 -- resolved address of CoreUObject::StaticLoadObject (all builds)
	// Scanned once on first call; result cached for all subsequent callers.
	static uintptr_t g_staticLoadObjectAddr    = 0;
	static bool      g_staticLoadObjectScanned = false;

	static uintptr_t HooksGetStaticLoadObjectAddress()
	{
		if (!g_staticLoadObjectScanned)
		{
			g_staticLoadObjectScanned = true;
			g_staticLoadObjectAddr = Scanner::FindPatternInMainModule(
				"StaticLoadObject", ScanPatterns::StaticLoadObject);
		}
		return g_staticLoadObjectAddr;
	}

	// v36 -- resolved addresses of CoreUObject object/package lookup and loading
	// functions, scanned during early modloader startup by Hooks::ObjectLookup.
	// Each thin wrapper just forwards the address resolved at startup.
	static uintptr_t HooksGetStaticFindObjectByPathAddress()     { return Hooks::ObjectLookup::GetStaticFindObject_ByPathAddress(); }
	static uintptr_t HooksGetStaticFindObjectByNameAddress()     { return Hooks::ObjectLookup::GetStaticFindObject_ByNameAddress(); }
	static uintptr_t HooksGetStaticFindObjectSafeByPathAddress() { return Hooks::ObjectLookup::GetStaticFindObjectSafe_ByPathAddress(); }
	static uintptr_t HooksGetStaticFindObjectSafeByNameAddress() { return Hooks::ObjectLookup::GetStaticFindObjectSafe_ByNameAddress(); }
	static uintptr_t HooksGetStaticFindObjectFastAddress()       { return Hooks::ObjectLookup::GetStaticFindObjectFastAddress(); }
	static uintptr_t HooksGetFindPackageAddress()                { return Hooks::ObjectLookup::GetFindPackageAddress(); }
	static uintptr_t HooksGetPackageFullyLoadAddress()           { return Hooks::ObjectLookup::GetUPackage_FullyLoadAddress(); }
	static uintptr_t HooksGetLoadPackageAddress()                { return Hooks::ObjectLookup::GetLoadPackageAddress(); }
	static uintptr_t HooksGetAssetDataFastGetAssetAddress()      { return Hooks::ObjectLookup::GetFAssetData_FastGetAssetAddress(); }

	// --- Text utilities ---

	static uintptr_t TextAsLocalizableAdvanced()
	{
		return Hooks::TextLocalization::GetOriginalPtr();
	}

	// Resolved address of SDK::UKismetTextLibrary::Conv_TextToString.
	// Scanned once on first call; result cached for all subsequent callers.
	static uintptr_t g_convTextToStringAddr    = 0;
	static bool      g_convTextToStringScanned = false;

	static uintptr_t TextConvTextToString()
	{
		if (!g_convTextToStringScanned)
		{
			g_convTextToStringScanned = true;
			g_convTextToStringAddr = Hooks::ResolveUFunctionNativeAddr("KismetTextLibrary", "Conv_TextToString");
		}
		return g_convTextToStringAddr;
	}

	static uintptr_t TextMakeTextKey()
	{
		return Hooks::TextKey::GetOriginalPtr();
	}

	static IPluginTextUtils g_textUtils = {
		TextAsLocalizableAdvanced,
		TextConvTextToString,
		TextMakeTextKey
	};

	// v18 -- game thread dispatch (all builds)
	static void HooksPostToGameThread(PluginGameThreadCallback fn, void* context)
	{
		if (!fn) return;
		GameThreadDispatch::PostVoid([fn, context]() { fn(context); });
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

	static void HooksRegisterCraftingFinishedCallback(void (*callback)(void*, void*, int32_t, int32_t))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterCraftingFinishedCallback: null callback");
			return;
		}

		Hooks::CraftingFinished::RegisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] CraftingFinished callback registered for plugin");
	}

	static void HooksUnregisterCraftingFinishedCallback(void (*callback)(void*, void*, int32_t, int32_t))
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterCraftingFinishedCallback: null callback");
			return;
		}

		Hooks::CraftingFinished::UnregisterPluginCallback(callback);
		LogDebug(L"[HooksInterface] CraftingFinished callback unregistered for plugin");
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
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnBeforeActivate: null callback");
			return;
		}
		Hooks::MassSpawnerActivate::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeActivate callback registered");
	}

	static void HooksUnregisterOnBeforeActivate(PluginBeforeActivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterOnBeforeActivate: null callback");
			return;
		}
		Hooks::MassSpawnerActivate::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeActivate callback unregistered");
	}

	static void HooksRegisterOnAfterActivate(PluginAfterActivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnAfterActivate: null callback");
			return;
		}
		Hooks::MassSpawnerActivate::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterActivate callback registered");
	}

	static void HooksUnregisterOnAfterActivate(PluginAfterActivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterOnAfterActivate: null callback");
			return;
		}
		Hooks::MassSpawnerActivate::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterActivate callback unregistered");
	}

	static void HooksRegisterOnBeforeDeactivate(PluginBeforeDeactivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnBeforeDeactivate: null callback");
			return;
		}
		Hooks::MassSpawnerDeactivate::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDeactivate callback registered");
	}

	static void HooksUnregisterOnBeforeDeactivate(PluginBeforeDeactivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterOnBeforeDeactivate: null callback");
			return;
		}
		Hooks::MassSpawnerDeactivate::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDeactivate callback unregistered");
	}

	static void HooksRegisterOnAfterDeactivate(PluginAfterDeactivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnAfterDeactivate: null callback");
			return;
		}
		Hooks::MassSpawnerDeactivate::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDeactivate callback registered");
	}

	static void HooksUnregisterOnAfterDeactivate(PluginAfterDeactivateSpawnerCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterOnAfterDeactivate: null callback");
			return;
		}
		Hooks::MassSpawnerDeactivate::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDeactivate callback unregistered");
	}

	static void HooksRegisterOnBeforeDoSpawning(PluginBeforeDoSpawningCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnBeforeDoSpawning: null callback");
			return;
		}
		Hooks::MassDoSpawning::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDoSpawning callback registered");
	}

	static void HooksUnregisterOnBeforeDoSpawning(PluginBeforeDoSpawningCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterOnBeforeDoSpawning: null callback");
			return;
		}
		Hooks::MassDoSpawning::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeDoSpawning callback unregistered");
	}

	static void HooksRegisterOnAfterDoSpawning(PluginAfterDoSpawningCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnAfterDoSpawning: null callback");
			return;
		}
		Hooks::MassDoSpawning::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDoSpawning callback registered");
	}

	static void HooksUnregisterOnAfterDoSpawning(PluginAfterDoSpawningCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] UnregisterOnAfterDoSpawning: null callback");
			return;
		}
		Hooks::MassDoSpawning::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterDoSpawning callback unregistered");
	}

	static void HooksRegisterOnBeforeWorldEndPlay(PluginWorldEndPlayCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnBeforeWorldEndPlay: null callback");
			return;
		}
		Hooks::WorldEndPlay::RegisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeWorldEndPlay callback registered");
	}

	static void HooksUnregisterOnBeforeWorldEndPlay(PluginWorldEndPlayCallback callback)
	{
		if (!callback) return;
		Hooks::WorldEndPlay::UnregisterBeforeCallback(callback);
		LogDebug(L"[HooksInterface] OnBeforeWorldEndPlay callback unregistered");
	}

	static void HooksRegisterOnAfterWorldEndPlay(PluginWorldEndPlayCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnAfterWorldEndPlay: null callback");
			return;
		}
		Hooks::WorldEndPlay::RegisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterWorldEndPlay callback registered");
	}

	static void HooksUnregisterOnAfterWorldEndPlay(PluginWorldEndPlayCallback callback)
	{
		if (!callback) return;
		Hooks::WorldEndPlay::UnregisterAfterCallback(callback);
		LogDebug(L"[HooksInterface] OnAfterWorldEndPlay callback unregistered");
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
		HooksUnregisterEngineTickCallback,
		HooksGetStaticLoadObjectAddress,  // v16
		HooksPostToGameThread,            // v18
		HooksGetStaticFindObjectByPathAddress,     // v36
		HooksGetStaticFindObjectByNameAddress,     // v36
		HooksGetStaticFindObjectSafeByPathAddress, // v36
		HooksGetStaticFindObjectSafeByNameAddress, // v36
		HooksGetStaticFindObjectFastAddress,       // v36
		HooksGetFindPackageAddress,                // v36
		HooksGetPackageFullyLoadAddress,           // v36
		HooksGetLoadPackageAddress,                // v36
		HooksGetAssetDataFastGetAssetAddress       // v36
	};

	static IPluginWorldEvents g_worldEvents = {
		HooksRegisterWorldBeginPlayCallback,
		HooksUnregisterWorldBeginPlayCallback,
		HooksRegisterAnyWorldBeginPlayCallback,
		HooksUnregisterAnyWorldBeginPlayCallback,
		HooksRegisterSaveLoadedCallback,
		HooksUnregisterSaveLoadedCallback,
		HooksRegisterExperienceLoadCompleteCallback,
		HooksUnregisterExperienceLoadCompleteCallback,
		HooksRegisterOnBeforeWorldEndPlay,
		HooksUnregisterOnBeforeWorldEndPlay,
		HooksRegisterOnAfterWorldEndPlay,
		HooksUnregisterOnAfterWorldEndPlay
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

	static IPluginCraftingEvents g_craftingEvents = {
		HooksRegisterCraftingFinishedCallback,
		HooksUnregisterCraftingFinishedCallback
	};

	// --- Input sub-interface wrappers (v15, client only) ---

#ifdef MODLOADER_CLIENT_BUILD
	static void HooksRegisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterKeybind: null callback");
			return;
		}
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

	static void HooksRegisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback || !combo)
		{
			LogWarn(L"[HooksInterface] RegisterKeybindByName: null argument");
			return;
		}
		Hooks::Input::RegisterKeybindByName(combo, event, callback);
		LogDebug(L"[HooksInterface] Keybind registered (combo=%S, event=%u)",
		         combo, static_cast<unsigned>(event));
	}

	static void HooksUnregisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback || !combo) return;
		Hooks::Input::UnregisterKeybindByName(combo, event, callback);
		LogDebug(L"[HooksInterface] Keybind unregistered (combo=%S, event=%u)",
		         combo, static_cast<unsigned>(event));
	}

	static void HooksRegisterKeybindCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterKeybindCombo: null callback");
			return;
		}
		Hooks::Input::RegisterKeybindCombo(key, mods, event, callback);
		LogDebug(L"[HooksInterface] Combo keybind registered (key=%u, mods=%u, event=%u)",
		         static_cast<unsigned>(key), static_cast<unsigned>(mods), static_cast<unsigned>(event));
	}

	static void HooksUnregisterKeybindCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback)
	{
		if (!callback) return;
		Hooks::Input::UnregisterKeybindCombo(key, mods, event, callback);
		LogDebug(L"[HooksInterface] Combo keybind unregistered (key=%u, mods=%u, event=%u)",
		         static_cast<unsigned>(key), static_cast<unsigned>(mods), static_cast<unsigned>(event));
	}

	// Input sub-interface struct (v15, extended v28/v29)
	static IPluginInputEvents g_inputEvents = {
		HooksRegisterKeybind,
		HooksUnregisterKeybind,
		HooksRegisterKeybindByName,   // v29: now handles plain keys and combos transparently
		HooksUnregisterKeybindByName,
		HooksRegisterKeybindCombo,    // v28: advanced — enum + mods, callback receives mods
		HooksUnregisterKeybindCombo   // v28
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
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnConfigChanged: null callback");
			return;
		}
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

	static WidgetHandle HooksRegisterWidget(const PluginWidgetDesc* desc)
	{
		return UI::PluginWidgetRegistry::RegisterWidget(desc);
	}

	static void HooksUnregisterWidget(WidgetHandle handle)
	{
		UI::PluginWidgetRegistry::UnregisterWidget(handle);
	}

	static void HooksSetWidgetVisible(WidgetHandle handle, bool visible)
	{
		UI::PluginWidgetRegistry::SetWidgetVisible(handle, visible);
	}

	// --- v43: panel-closed notifications & input capture ---

	static void HooksRegisterOnPanelWindowClosed(PluginPanelClosedCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnPanelWindowClosed: null callback");
			return;
		}
		UI::PluginPanelRegistry::RegisterOnPanelWindowClosed(callback);
	}

	static void HooksUnregisterOnPanelWindowClosed(PluginPanelClosedCallback callback)
	{
		if (!callback) return;
		UI::PluginPanelRegistry::UnregisterOnPanelWindowClosed(callback);
	}

	static void* HooksAcquireInputCapture()
	{
		return UI::PluginPanelRegistry::AcquireInputCapture();
	}

	static void HooksReleaseInputCapture(void* token)
	{
		UI::PluginPanelRegistry::ReleaseInputCapture(token);
	}

	// UI sub-interface struct (v16, extended v43)
	static IPluginUIEvents g_uiEvents = {
		HooksRegisterPanel,
		HooksUnregisterPanel,
		HooksRegisterOnConfigChanged,
		HooksUnregisterOnConfigChanged,
		HooksSetPanelOpen,
		HooksSetPanelClose,
		HooksRegisterWidget,       // v16
		HooksUnregisterWidget,     // v16
		HooksSetWidgetVisible,     // v16
		HooksRegisterOnPanelWindowClosed,   // v43
		HooksUnregisterOnPanelWindowClosed, // v43
		HooksAcquireInputCapture,           // v43
		HooksReleaseInputCapture            // v43
	};

	// --- HUD sub-interface wrappers (v16, client only) ---

	static void HooksRegisterOnPostRender(PluginHUDPostRenderCallback cb)
	{
		Hooks::HUDPostRender::RegisterPluginCallback(cb);
	}

	static void HooksUnregisterOnPostRender(PluginHUDPostRenderCallback cb)
	{
		Hooks::HUDPostRender::UnregisterPluginCallback(cb);
	}

	static uintptr_t HooksGetGatherPlayersDataAddress()
	{
		return Hooks::HUDPostRender::GetGatherPlayersDataAddress();
	}

	// HUD sub-interface struct (v16)
	static IPluginHUDEvents g_hudEvents = {
		HooksRegisterOnPostRender,
		HooksUnregisterOnPostRender,
		HooksGetGatherPlayersDataAddress
	};

	// --- Splash feedback wrappers (v40, client only) ---

	static bool SplashIsVisible()
	{
		return Splash::IsVisible();
	}

	static void SplashSetSubStatus(const char* text)
	{
		if (!text) return;
		wchar_t buf[256];
		MultiByteToWideChar(CP_UTF8, 0, text, -1, buf, 256);
		Splash::SetSubStatus(buf);
	}

	static void SplashSetSubProgress(float fraction)
	{
		Splash::SetSubProgress(fraction);
	}

	static void SplashClearSubBar()
	{
		Splash::ClearSubBar();
	}

	static void SplashAcquireHold()
	{
		Splash::AcquireHold();
	}

	static void SplashReleaseHold()
	{
		Splash::ReleaseHold();
	}

	static IPluginSplash g_splashInterface = {
		SplashIsVisible,
		SplashSetSubStatus,
		SplashSetSubProgress,
		SplashClearSubBar,
		SplashAcquireHold,
		SplashReleaseHold
	};

#endif // MODLOADER_CLIENT_BUILD

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
	// --- Net mode info wrappers (v36) ---

	static EPluginNetMode HooksGetNetMode()
	{
		return Hooks::SessionInfo::GetNetMode();
	}

	static bool HooksNetModeIsMultiplayer()
	{
		return Hooks::SessionInfo::IsMultiplayer();
	}

	static bool HooksNetModeIsServer()
	{
		return Hooks::SessionInfo::IsServer();
	}

	// Net mode info sub-interface struct (v36)
	static IPluginNetModeInfo g_netModeInfo = {
		HooksGetNetMode,
		HooksNetModeIsMultiplayer,
		HooksNetModeIsServer
	};
#endif // MODLOADER_SERVER_BUILD || MODLOADER_CLIENT_BUILD

	// --- Native pointer wrappers (v21) ---

	static uintptr_t NativeEngineLoopInit()   { return Hooks::EngineInit::GetOriginalPtrEngineLoopInit(); }
	static uintptr_t NativeGameEngineInit()   { return Hooks::EngineInit::GetOriginalPtrGameEngineInit(); }
	static uintptr_t NativeEngineLoopExit()   { return Hooks::EngineShutdown::GetOriginalPtrEngineLoopExit(); }
	static uintptr_t NativeEnginePreExit()  { return Hooks::EngineShutdown::GetOriginalPtrEnginePreExit(); }
	static uintptr_t NativeEngineTick()  { return Hooks::EngineTick::GetOriginalPtr(); }
	static uintptr_t NativeWorldBeginPlay()   { return Hooks::WorldBeginPlay::GetOriginalPtr(); }
	static uintptr_t NativeWorldEndPlay()     { return Hooks::WorldEndPlay::GetOriginalPtr(); }
	static uintptr_t NativeSaveLoaded()       { return Hooks::SaveLoaded::GetOriginalPtr(); }
	static uintptr_t NativeExperienceLoadComplete() { return Hooks::ExperienceLoadComplete::GetOriginalPtr(); }
	static uintptr_t NativeActorBeginPlay()   { return Hooks::ActorBeginPlay::GetOriginalPtr(); }
	static uintptr_t NativePlayerJoined()     { return Hooks::PlayerJoined::GetOriginalPtr(); }
	static uintptr_t NativePlayerLeft()       { return Hooks::PlayerLeft::GetOriginalPtr(); }
	static uintptr_t NativeSpawnerActivate()  { return Hooks::MassSpawnerActivate::GetOriginalPtr(); }
	static uintptr_t NativeSpawnerDeactivate(){ return Hooks::MassSpawnerDeactivate::GetOriginalPtr(); }
	static uintptr_t NativeSpawnerDoSpawning(){ return Hooks::MassDoSpawning::GetOriginalPtr(); }
	static uintptr_t NativeCraftingFinished() { return Hooks::CraftingFinished::GetOriginalPtr(); }
#ifdef MODLOADER_CLIENT_BUILD
	static uintptr_t NativeHUDPostRender()    { return Hooks::HUDPostRender::GetOriginalPtr(); }
	static uintptr_t NativeClientMessageExec(){ return Hooks::ClientMessage::GetOriginalPtr(); }
#endif

	// Native pointers sub-interface struct (v21)
	static IPluginNativePointers g_nativePointers = {
		NativeEngineLoopInit,
		NativeGameEngineInit,
		NativeEngineLoopExit,
		NativeEnginePreExit,
		NativeEngineTick,
		NativeWorldBeginPlay,
		NativeWorldEndPlay,
		NativeSaveLoaded,
		NativeExperienceLoadComplete,
		NativeActorBeginPlay,
		NativePlayerJoined,
		NativePlayerLeft,
		NativeSpawnerActivate,
		NativeSpawnerDeactivate,
		NativeSpawnerDoSpawning,
#ifdef MODLOADER_CLIENT_BUILD
		NativeHUDPostRender,      // client only
		NativeClientMessageExec,  // client only
#else
		nullptr,        // HUDPostRender — null on server/generic builds
		nullptr,         // ClientMessageExec — null on server/generic builds
#endif
		NativeCraftingFinished,  // v44 -- appended at end to preserve layout for v42/v43 plugins
	};

	// -----------------------------------------------------------------------
	// IPluginHttpServer wrappers (v22, server only)
	// -----------------------------------------------------------------------

#ifdef MODLOADER_SERVER_BUILD
	static bool HooksHttpServerAddRoute(const IPluginSelf* self, const char* folderName)
	{
		if (!self || !self->name || !folderName)
		{
			LogWarn(L"[HooksInterface] HttpServer::AddRoute: null argument");
			return false;
		}
		return Hooks::HttpServer::AddRoute(self->name, folderName);
	}

	static void HooksHttpServerRemoveRoute(const IPluginSelf* self, const char* folderName)
	{
		if (!self || !self->name || !folderName) return;
		Hooks::HttpServer::RemoveRoute(self->name, folderName);
	}

	static void HooksHttpServerRegisterOnRawRequest(PluginHttpRequestFilterCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] HttpServer::RegisterOnRawRequest: null callback");
			return;
		}
		Hooks::HttpServer::RegisterRawRequestFilter(callback);
	}

	static void HooksHttpServerUnregisterOnRawRequest(PluginHttpRequestFilterCallback callback)
	{
		if (!callback) return;
		Hooks::HttpServer::UnregisterRawRequestFilter(callback);
	}

	static bool HooksHttpServerAddRawRoute(const IPluginSelf* self, const char* urlPrefix,
	      PluginHttpRouteCallback callback)
	{
		if (!self || !self->name || !urlPrefix || !callback)
		{
			LogWarn(L"[HooksInterface] HttpServer::AddRawRoute: null argument");
			return false;
		}
		return Hooks::HttpServer::AddRawRoute(self->name, urlPrefix, callback);
	}

	static void HooksHttpServerRemoveRawRoute(const IPluginSelf* self, const char* urlPrefix)
	{
		if (!self || !self->name || !urlPrefix) return;
		Hooks::HttpServer::RemoveRawRoute(self->name, urlPrefix);
	}

	static IPluginHttpServer g_httpServer = {
		HooksHttpServerAddRoute,
		HooksHttpServerRemoveRoute,
		HooksHttpServerRegisterOnRawRequest,
		HooksHttpServerUnregisterOnRawRequest,
		HooksHttpServerAddRawRoute,
		HooksHttpServerRemoveRawRoute,
	};
#endif // MODLOADER_SERVER_BUILD

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
		&g_inputEvents,      // v15 — keybind events (client only)
		&g_uiEvents,         // v15 — custom panel + config-change callbacks (client only)
		&g_hudEvents,        // v16 — AHUD::PostRender callbacks + HUD function addresses (client only)
#else
		nullptr,             // v15 — Input is null on server/generic builds
		nullptr,             // v15 — UI is null on server/generic builds
		nullptr,             // v16 — HUD is null on server/generic builds
#endif
		nullptr,             // v17 — Network; filled in below by GetPluginHooks()
		&g_nativePointers,   // v21 — trampoline addresses for all managed hooks
#ifdef MODLOADER_SERVER_BUILD
		&g_httpServer,       // v22 — HTTP static-file routes + raw-request filters (server only)
#else
		nullptr,             // v22 — HttpServer is null on client/generic builds
#endif
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
		&g_netModeInfo,      // v36 — net mode query functions (server + client)
#else
		nullptr,             // v36 — NetMode is null on generic builds
#endif
		&g_textUtils,         // FText localization helpers (AsLocalizable_Advanced, Conv_TextToString)
#ifdef MODLOADER_CLIENT_BUILD
		nullptr,              // v37 — ImGuiTextures; filled in below by GetPluginHooks()
		&g_splashInterface,   // v40 — splash feedback (client only)
#else
		nullptr,              // v37 — ImGuiTextures is null on server/generic builds
		nullptr,              // v40 — Splash is null on server/generic builds
#endif
		&g_craftingEvents,   // v44 — appended at end to preserve layout for v42/v43 plugins
	};
	static bool g_networkChannelInitialized = false;

	IPluginHooks* GetPluginHooks()
	{
		// Resolve the network channel pointer on first call.
		// NetworkChannel::GetInterface() returns nullptr on generic builds.
		if (!g_pluginHooks.Network)
		{
			g_pluginHooks.Network = NetworkChannel::GetInterface();
			if (g_pluginHooks.Network && !g_networkChannelInitialized)
			{
				NetworkChannel::Initialize();
				g_networkChannelInitialized = true;
			}
		}
#ifdef MODLOADER_CLIENT_BUILD
		// Resolve the ImGui texture API pointer on first call (valid after Initialize).
		if (!g_pluginHooks.ImGuiTextures)
			g_pluginHooks.ImGuiTextures = UI::ImGuiBackend::GetTextureAPI();
#endif
		return &g_pluginHooks;
	}
}
