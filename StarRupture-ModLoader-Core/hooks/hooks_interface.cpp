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
#include "console/plugin_console.h"
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
#include "hooks/game/debug_draw/debug_draw.h"
#endif
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#include "hooks/game/session_info/session_info.h"
#endif
#include "hooks/game/object_lookup/object_lookup.h"
#include "hooks/game/gobject_walk/gobject_walk.h"
#include "hooks/game/object_properties/object_properties.h"
#include "hooks/game/delegate_hook/delegate_hook.h"
#include <unordered_map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

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

	// --- Object walker sub-interface wrappers (v47) ---

	// Translates the plugin-facing PluginObjectLookupMode (int32 enum, stable
	// ABI value) to the internal Hooks::GObjectWalk::LookupMode.
	static Hooks::GObjectWalk::LookupMode ToInternalLookupMode(PluginObjectLookupMode mode)
	{
		switch (mode)
		{
		case PluginObjectLookup_InstanceOnly:
			return Hooks::GObjectWalk::LookupMode::InstanceOnly;
		case PluginObjectLookup_CDOOnly:
			return Hooks::GObjectWalk::LookupMode::CDOOnly;
		case PluginObjectLookup_Both:
		default:
			return Hooks::GObjectWalk::LookupMode::Both;
		}
	}

	// Fills pluginArray (capacity entries) from internalArray (same count).
	// The two ObjectInfo/PluginObjectInfo structs have identical layout aside
	// from the object pointer's type and are filled in lockstep up to capacity.
	static void CopyObjectInfos(const Hooks::GObjectWalk::ObjectInfo* internalArray, int count,
		PluginObjectInfo* pluginArray)
	{
		for (int i = 0; i < count; ++i)
		{
			pluginArray[i].object = internalArray[i].object;
			strncpy_s(pluginArray[i].className, internalArray[i].className, _TRUNCATE);
			strncpy_s(pluginArray[i].objectName, internalArray[i].objectName, _TRUNCATE);
			pluginArray[i].nameNumber = internalArray[i].nameNumber;
			pluginArray[i].objectFlags = internalArray[i].objectFlags;
			pluginArray[i].objectIndex = internalArray[i].objectIndex;
		}
	}

	static bool ObjectWalkerIsReady()
	{
		return Hooks::GObjectWalk::IsReady();
	}

	static int ObjectWalkerWalkAllObjectsInto(PluginObjectLookupMode mode, PluginObjectInfo* outArray, int capacity)
	{
		if (!outArray || capacity <= 0)
			return Hooks::GObjectWalk::WalkAllInto(ToInternalLookupMode(mode), nullptr, 0);

		std::vector<Hooks::GObjectWalk::ObjectInfo> scratch(static_cast<size_t>(capacity));
		int total = Hooks::GObjectWalk::WalkAllInto(ToInternalLookupMode(mode), scratch.data(), capacity);
		CopyObjectInfos(scratch.data(), std::min(total, capacity), outArray);
		return total;
	}

	static int ObjectWalkerFindObjectsByClassNameInto(const char* className, PluginObjectLookupMode mode,
		PluginObjectInfo* outArray, int capacity)
	{
		if (!className)
			return 0;

		if (!outArray || capacity <= 0)
			return Hooks::GObjectWalk::FindObjectsByClassNameInto(className, ToInternalLookupMode(mode), nullptr, 0);

		std::vector<Hooks::GObjectWalk::ObjectInfo> scratch(static_cast<size_t>(capacity));
		int total = Hooks::GObjectWalk::FindObjectsByClassNameInto(className, ToInternalLookupMode(mode), scratch.data(), capacity);
		CopyObjectInfos(scratch.data(), std::min(total, capacity), outArray);
		return total;
	}

	static void* ObjectWalkerFindFirstObjectByName(const char* objectName)
	{
		return Hooks::GObjectWalk::FindFirstObjectByName(objectName);
	}

	static int ObjectWalkerFindObjectsByNameInto(const char* objectName, PluginObjectLookupMode mode,
		PluginObjectInfo* outArray, int capacity)
	{
		if (!objectName)
			return 0;

		if (!outArray || capacity <= 0)
			return Hooks::GObjectWalk::FindObjectsByNameInto(objectName, ToInternalLookupMode(mode), nullptr, 0);

		std::vector<Hooks::GObjectWalk::ObjectInfo> scratch(static_cast<size_t>(capacity));
		int total = Hooks::GObjectWalk::FindObjectsByNameInto(objectName, ToInternalLookupMode(mode), scratch.data(), capacity);
		CopyObjectInfos(scratch.data(), std::min(total, capacity), outArray);
		return total;
	}

	static bool ObjectWalkerInvokeUFunctionByName(void* object, const char* className, const char* funcName, void* paramsBuffer)
	{
		return Hooks::GObjectWalk::InvokeUFunctionByName(
			static_cast<SDK::UObject*>(object), className, funcName, paramsBuffer);
	}

	static void* ObjectWalkerResolveUFunction(const char* className, const char* funcName)
	{
		return Hooks::GObjectWalk::ResolveUFunction(className, funcName);
	}

	static bool ObjectWalkerInvokeResolvedUFunction(void* object, void* resolvedFunction, void* paramsBuffer)
	{
		return Hooks::GObjectWalk::InvokeResolvedUFunction(
			static_cast<SDK::UObject*>(object), static_cast<SDK::UFunction*>(resolvedFunction), paramsBuffer);
	}

	static IPluginObjectWalker g_objectWalker = {
		ObjectWalkerIsReady,
		ObjectWalkerWalkAllObjectsInto,
		ObjectWalkerFindObjectsByClassNameInto,
		ObjectWalkerFindFirstObjectByName,
		ObjectWalkerFindObjectsByNameInto,
		ObjectWalkerInvokeUFunctionByName,
		ObjectWalkerResolveUFunction,
		ObjectWalkerInvokeResolvedUFunction
	};

	// --- DelegateHook sub-interface wrappers (v47) ---

	static DelegateHookHandle DelegateHookHook(void* delegatePtr, void* hostObject,
		const char* hostClassName, const char* hostFuncName,
		PluginDelegateCallback callback, void* userContext)
	{
		return static_cast<DelegateHookHandle>(Hooks::DelegateHook::HookViaExistingUFunction(
			delegatePtr, static_cast<SDK::UObject*>(hostObject),
			reinterpret_cast<Hooks::DelegateHook::DelegateCallback>(callback), userContext,
			hostClassName, hostFuncName));
	}

	static bool DelegateHookUnhook(DelegateHookHandle handle)
	{
		return Hooks::DelegateHook::Unhook(static_cast<Hooks::DelegateHook::HookHandle>(handle));
	}

	static bool DelegateHookIsHooked(DelegateHookHandle handle)
	{
		return Hooks::DelegateHook::IsHooked(static_cast<Hooks::DelegateHook::HookHandle>(handle));
	}

	static IPluginDelegateHook g_delegateHook = {
		DelegateHookHook,
		DelegateHookUnhook,
		DelegateHookIsHooked
	};

	// --- ObjectProperties sub-interface wrappers (v47) ---

	static bool ObjectPropertiesIsReady()
	{
		return Hooks::ObjectProperties::IsReady();
	}

	static PluginPropertyHandle ObjectPropertiesFindPropertyByName(const char* className, const char* propertyName)
	{
		return static_cast<PluginPropertyHandle>(Hooks::ObjectProperties::ResolveProperty(className, propertyName));
	}

	static PluginPropertyHandle ObjectPropertiesFindPropertyOnObject(void* object, const char* propertyName)
	{
		return static_cast<PluginPropertyHandle>(
			Hooks::ObjectProperties::FindPropertyOnObject(static_cast<SDK::UObject*>(object), propertyName));
	}

	static PluginPropertyKind ObjectPropertiesGetPropertyKind(PluginPropertyHandle property)
	{
		switch (Hooks::ObjectProperties::GetPropertyKind(static_cast<SDK::FProperty*>(property)))
		{
		case Hooks::ObjectProperties::PropertyKind::Bool:        return PluginPropertyKind::Bool;
		case Hooks::ObjectProperties::PropertyKind::Int:         return PluginPropertyKind::Int;
		case Hooks::ObjectProperties::PropertyKind::Float:       return PluginPropertyKind::Float;
		case Hooks::ObjectProperties::PropertyKind::Name:        return PluginPropertyKind::Name;
		case Hooks::ObjectProperties::PropertyKind::Str:         return PluginPropertyKind::Str;
		case Hooks::ObjectProperties::PropertyKind::Object:      return PluginPropertyKind::Object;
		case Hooks::ObjectProperties::PropertyKind::Struct:      return PluginPropertyKind::Struct;
		case Hooks::ObjectProperties::PropertyKind::Array:       return PluginPropertyKind::Array;
		case Hooks::ObjectProperties::PropertyKind::Enum:        return PluginPropertyKind::Enum;
		case Hooks::ObjectProperties::PropertyKind::Unsupported: return PluginPropertyKind::Unsupported;
		default:                                                 return PluginPropertyKind::Unknown;
		}
	}

	static size_t ObjectPropertiesGetPropertySize(PluginPropertyHandle property)
	{
		return Hooks::ObjectProperties::GetPropertySize(static_cast<SDK::FProperty*>(property));
	}

	static int32_t ObjectPropertiesGetPropertyArrayDim(PluginPropertyHandle property)
	{
		return Hooks::ObjectProperties::GetPropertyArrayDim(static_cast<SDK::FProperty*>(property));
	}

	static void* ObjectPropertiesGetPropertyRawPtr(void* container, PluginPropertyHandle property)
	{
		return Hooks::ObjectProperties::GetPropertyRawPtr(container, static_cast<SDK::FProperty*>(property));
	}

	static bool ObjectPropertiesGetBoolProperty(void* container, PluginPropertyHandle property, bool* outValue)
	{
		return Hooks::ObjectProperties::GetBoolProperty(container, static_cast<SDK::FProperty*>(property), outValue);
	}

	static bool ObjectPropertiesSetBoolProperty(void* container, PluginPropertyHandle property, bool value)
	{
		return Hooks::ObjectProperties::SetBoolProperty(container, static_cast<SDK::FProperty*>(property), value);
	}

	static bool ObjectPropertiesGetIntProperty(void* container, PluginPropertyHandle property, int64_t* outValue)
	{
		return Hooks::ObjectProperties::GetIntProperty(container, static_cast<SDK::FProperty*>(property), outValue);
	}

	static bool ObjectPropertiesSetIntProperty(void* container, PluginPropertyHandle property, int64_t value)
	{
		return Hooks::ObjectProperties::SetIntProperty(container, static_cast<SDK::FProperty*>(property), value);
	}

	static bool ObjectPropertiesGetFloatProperty(void* container, PluginPropertyHandle property, double* outValue)
	{
		return Hooks::ObjectProperties::GetFloatProperty(container, static_cast<SDK::FProperty*>(property), outValue);
	}

	static bool ObjectPropertiesSetFloatProperty(void* container, PluginPropertyHandle property, double value)
	{
		return Hooks::ObjectProperties::SetFloatProperty(container, static_cast<SDK::FProperty*>(property), value);
	}

	static bool ObjectPropertiesGetObjectProperty(void* container, PluginPropertyHandle property, void** outValue)
	{
		SDK::UObject* obj = nullptr;
		if (!Hooks::ObjectProperties::GetObjectProperty(container, static_cast<SDK::FProperty*>(property), &obj))
			return false;
		*outValue = obj;
		return true;
	}

	static bool ObjectPropertiesSetObjectProperty(void* container, PluginPropertyHandle property, void* value)
	{
		return Hooks::ObjectProperties::SetObjectProperty(
			container, static_cast<SDK::FProperty*>(property), static_cast<SDK::UObject*>(value));
	}

	static bool ObjectPropertiesGetStringProperty(void* container, PluginPropertyHandle property, char* outBuffer, int bufferSize)
	{
		return Hooks::ObjectProperties::GetStringProperty(container, static_cast<SDK::FProperty*>(property), outBuffer, bufferSize);
	}

	static bool ObjectPropertiesGetNameProperty(void* container, PluginPropertyHandle property, char* outBuffer, int bufferSize)
	{
		return Hooks::ObjectProperties::GetNameProperty(container, static_cast<SDK::FProperty*>(property), outBuffer, bufferSize);
	}

	static bool ObjectPropertiesGetPropertyStructTypeName(PluginPropertyHandle property, char* outBuffer, int bufferSize)
	{
		if (!outBuffer || bufferSize <= 0)
			return false;

		SDK::UStruct* structType = Hooks::ObjectProperties::GetPropertyStructType(static_cast<SDK::FProperty*>(property));
		if (!structType)
		{
			outBuffer[0] = '\0';
			return false;
		}

		std::string name = structType->GetName();
		strncpy_s(outBuffer, static_cast<size_t>(bufferSize), name.c_str(), _TRUNCATE);
		return true;
	}

	static IPluginObjectProperties g_objectProperties = {
		ObjectPropertiesIsReady,
		ObjectPropertiesFindPropertyByName,
		ObjectPropertiesFindPropertyOnObject,
		ObjectPropertiesGetPropertyKind,
		ObjectPropertiesGetPropertySize,
		ObjectPropertiesGetPropertyArrayDim,
		ObjectPropertiesGetPropertyRawPtr,
		ObjectPropertiesGetBoolProperty,
		ObjectPropertiesSetBoolProperty,
		ObjectPropertiesGetIntProperty,
		ObjectPropertiesSetIntProperty,
		ObjectPropertiesGetFloatProperty,
		ObjectPropertiesSetFloatProperty,
		ObjectPropertiesGetObjectProperty,
		ObjectPropertiesSetObjectProperty,
		ObjectPropertiesGetStringProperty,
		ObjectPropertiesGetNameProperty,
		ObjectPropertiesGetPropertyStructTypeName,
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

	static void HooksRegisterOnConfigChanged(const IPluginSelf* self, PluginConfigChangedCallback callback)
	{
		if (!callback)
		{
			LogWarn(L"[HooksInterface] RegisterOnConfigChanged: null callback");
			return;
		}
		UI::PluginPanelRegistry::RegisterOnConfigChanged(self, callback);
	}

	static void HooksUnregisterOnConfigChanged(const IPluginSelf* self, PluginConfigChangedCallback callback)
	{
		if (!callback) return;
		UI::PluginPanelRegistry::UnregisterOnConfigChanged(self, callback);
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

	static void* HooksAcquireInputPassthrough()
	{
		return UI::PluginPanelRegistry::AcquireInputPassthrough();
	}

	static void HooksReleaseInputPassthrough(void* token)
	{
		UI::PluginPanelRegistry::ReleaseInputPassthrough(token);
	}

	// UI sub-interface struct (v16, extended v43/v51)
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
		HooksReleaseInputCapture,           // v43
		HooksAcquireInputPassthrough,       // v51
		HooksReleaseInputPassthrough        // v51
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

	// --- Debug draw wrappers (v50, client only) ---
	//
	// The plugin-facing geometry structs and the core ones in debug_draw.h have
	// the same shape but are deliberately kept as separate declarations, so
	// these convert field by field rather than casting across. Every pointer
	// argument is treated as optional -- a plugin passing null gets a zeroed
	// value rather than a fault.

	namespace DD = Hooks::DebugDraw;

	static DD::DVec ToVec(const PluginDebugVector* v)
	{
		if (!v) return DD::DVec{ 0.0, 0.0, 0.0 };
		return DD::DVec{ v->x, v->y, v->z };
	}

	static DD::DRot ToRot(const PluginDebugRotator* r)
	{
		if (!r) return DD::DRot{ 0.0, 0.0, 0.0 };
		return DD::DRot{ r->pitch, r->yaw, r->roll };
	}

	static DD::DColor ToColor(const PluginDebugColor* c)
	{
		if (!c) return DD::DColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		return DD::DColor{ c->r, c->g, c->b, c->a };
	}

	static DD::DPlane ToPlane(const PluginDebugPlane* p)
	{
		if (!p) return DD::DPlane{ 0.0, 0.0, 1.0, 0.0 };
		return DD::DPlane{ p->x, p->y, p->z, p->w };
	}

	static DD::DTransform ToTransform(const PluginDebugTransform* t)
	{
		DD::DTransform out{};
		out.scale = DD::DVec{ 1.0, 1.0, 1.0 };
		if (!t) return out;
		out.location = ToVec(&t->location);
		out.rotation = ToRot(&t->rotation);
		out.scale    = ToVec(&t->scale);
		return out;
	}

	static DD::DStyle ToStyle(const PluginDebugDrawStyle* s)
	{
		DD::DStyle out{};
		out.color = DD::DColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		if (!s) return out;
		out.color       = ToColor(&s->color);
		out.duration    = s->duration;
		out.thickness   = s->thickness;
		out.bPersistent = s->bPersistent;
		out.bForeground = s->bForeground;
		return out;
	}

	// The line batchers are UObjects and must only be touched on the game
	// thread -- but the most natural place for a plugin to want to draw
	// something is inside its ImGui panel/widget callback, and those run on the
	// render thread (the modloader draws from the Present hook). Rather than
	// make that a documented footgun, every entry point below is safe from any
	// thread: arguments are converted to owned values up front, then the draw
	// either runs inline or is deferred one frame via GameThreadDispatch.
	//
	// Nothing plugin-owned is retained past the call -- including the
	// float-history sample array, which is deep-copied. The one exception is
	// actor pointers (DrawCamera's camera actor, DrawString's base actor);
	// those are passed through as-is and must stay valid for the frame, which
	// is the same expectation the engine has of them.
	template <typename Fn>
	static void OnGameThread(Fn&& fn)
	{
		if (GameThreadDispatch::IsGameThread())
			fn();
		else
			GameThreadDispatch::PostVoid(std::forward<Fn>(fn));
	}

	static bool DebugDrawIsAvailable()
	{
		return DD::Resolve();
	}

	static void DebugDrawLine(const PluginDebugVector* start, const PluginDebugVector* end,
	                          const PluginDebugDrawStyle* style)
	{
		const DD::DVec   a = ToVec(start);
		const DD::DVec   b = ToVec(end);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([a, b, s]() { DD::DrawLine(a, b, s); });
	}

	static void DebugDrawPoint(const PluginDebugVector* position, float size,
	                           const PluginDebugDrawStyle* style)
	{
		const DD::DVec   p = ToVec(position);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([p, size, s]() { DD::DrawPoint(p, size, s); });
	}

	static void DebugDrawCircle(const PluginDebugVector* center, float radius, int numSegments,
	                            const PluginDebugVector* yAxis, const PluginDebugVector* zAxis,
	                            bool bDrawAxis, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   c = ToVec(center);
		const DD::DVec   y = yAxis ? ToVec(yAxis) : DD::DVec{ 0.0, 1.0, 0.0 };
		const DD::DVec   z = zAxis ? ToVec(zAxis) : DD::DVec{ 0.0, 0.0, 1.0 };
		const DD::DStyle s = ToStyle(style);
		OnGameThread([c, radius, numSegments, y, z, bDrawAxis, s]()
		{
			DD::DrawCircle(c, radius, numSegments, y, z, bDrawAxis, s);
		});
	}

	static void DebugDrawSphere(const PluginDebugVector* center, float radius, int segments,
	                            const PluginDebugDrawStyle* style)
	{
		const DD::DVec   c = ToVec(center);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([c, radius, segments, s]() { DD::DrawSphere(c, radius, segments, s); });
	}

	static void DebugDrawBox(const PluginDebugVector* center, const PluginDebugVector* extent,
	                         const PluginDebugRotator* rotation, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   c = ToVec(center);
		const DD::DVec   e = ToVec(extent);
		const DD::DRot   r = ToRot(rotation);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([c, e, r, s]() { DD::DrawBox(c, e, r, s); });
	}

	static void DebugDrawCapsule(const PluginDebugVector* center, float halfHeight, float radius,
	                             const PluginDebugRotator* rotation, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   c = ToVec(center);
		const DD::DRot   r = ToRot(rotation);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([c, halfHeight, radius, r, s]()
		{
			DD::DrawCapsule(c, halfHeight, radius, r, s);
		});
	}

	static void DebugDrawCylinder(const PluginDebugVector* start, const PluginDebugVector* end,
	                              float radius, int segments, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   a = ToVec(start);
		const DD::DVec   b = ToVec(end);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([a, b, radius, segments, s]() { DD::DrawCylinder(a, b, radius, segments, s); });
	}

	static void DebugDrawConeInDegrees(const PluginDebugVector* origin, const PluginDebugVector* direction,
	                                   float length, float angleWidthDeg, float angleHeightDeg,
	                                   int numSides, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   o   = ToVec(origin);
		const DD::DVec   dir = direction ? ToVec(direction) : DD::DVec{ 1.0, 0.0, 0.0 };
		const DD::DStyle s   = ToStyle(style);
		OnGameThread([o, dir, length, angleWidthDeg, angleHeightDeg, numSides, s]()
		{
			DD::DrawConeInDegrees(o, dir, length, angleWidthDeg, angleHeightDeg, numSides, s);
		});
	}

	static void DebugDrawArrow(const PluginDebugVector* start, const PluginDebugVector* end,
	                           float arrowSize, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   a = ToVec(start);
		const DD::DVec   b = ToVec(end);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([a, b, arrowSize, s]() { DD::DrawArrow(a, b, arrowSize, s); });
	}

	static void DebugDrawCoordinateSystem(const PluginDebugVector* location, const PluginDebugRotator* rotation,
	                                      float scale, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   l = ToVec(location);
		const DD::DRot   r = ToRot(rotation);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([l, r, scale, s]() { DD::DrawCoordinateSystem(l, r, scale, s); });
	}

	static void DebugDrawPlane(const PluginDebugPlane* plane, const PluginDebugVector* location,
	                           float size, const PluginDebugDrawStyle* style)
	{
		const DD::DPlane p = ToPlane(plane);
		const DD::DVec   l = ToVec(location);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([p, l, size, s]() { DD::DrawPlane(p, l, size, s); });
	}

	static void DebugDrawFrustum(const PluginDebugTransform* frustumTransform,
	                             const PluginDebugDrawStyle* style)
	{
		const DD::DTransform t = ToTransform(frustumTransform);
		const DD::DStyle     s = ToStyle(style);
		OnGameThread([t, s]() { DD::DrawFrustum(t, s); });
	}

	static void DebugDrawCamera(void* cameraActor, float scale, const PluginDebugDrawStyle* style)
	{
		const DD::DStyle s = ToStyle(style);
		OnGameThread([cameraActor, scale, s]() { DD::DrawCamera(cameraActor, scale, s); });
	}

	static void DebugDrawCameraAt(const PluginDebugVector* location, const PluginDebugRotator* rotation,
	                              float fovDegrees, float scale, const PluginDebugDrawStyle* style)
	{
		const DD::DVec   l = ToVec(location);
		const DD::DRot   r = ToRot(rotation);
		const DD::DStyle s = ToStyle(style);
		OnGameThread([l, r, fovDegrees, scale, s]() { DD::DrawCameraAt(l, r, fovDegrees, scale, s); });
	}

	static void DebugDrawString(const PluginDebugVector* location, const char* text, void* testBaseActor,
	                            const PluginDebugColor* color, float duration, float fontScale)
	{
		if (!text || !text[0])
			return;

		wchar_t wide[1024];
		if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 1024) == 0)
			return;

		const DD::DVec   l = ToVec(location);
		const DD::DColor c = ToColor(color);
		std::wstring     owned(wide);

		OnGameThread([l, c, owned = std::move(owned), testBaseActor, duration, fontScale]()
		{
			DD::DrawString(l, owned.c_str(), testBaseActor, c, duration, fontScale);
		});
	}

	// Deep-copies the plugin's sample array: the draw can land a frame late and
	// the caller is free to reuse or free its buffer the moment we return.
	static void DebugDrawFloatHistoryDeferred(const PluginDebugFloatHistory* history,
	                                          const DD::DTransform& transform,
	                                          const DD::DVec& drawSize,
	                                          const DD::DStyle& style,
	                                          bool bUseTransform)
	{
		std::vector<float> samples;
		float minValue = 0.0f;
		float maxValue = 0.0f;
		bool  bAuto    = false;

		if (history)
		{
			if (history->samples && history->count > 0)
				samples.assign(history->samples, history->samples + history->count);
			minValue = history->minValue;
			maxValue = history->maxValue;
			bAuto    = history->bAutoAdjustMinMax;
		}

		OnGameThread([samples = std::move(samples), transform, drawSize, style,
		              minValue, maxValue, bAuto, bUseTransform]()
		{
			DD::DFloatHistory h{};
			h.samples           = samples.data();
			h.count             = static_cast<int32_t>(samples.size());
			h.minValue          = minValue;
			h.maxValue          = maxValue;
			h.bAutoAdjustMinMax = bAuto;

			if (bUseTransform)
				DD::DrawFloatHistoryTransform(h, transform, drawSize, style);
			else
				DD::DrawFloatHistoryLocation(h, transform.location, drawSize, style);
		});
	}

	static void DebugDrawFloatHistoryTransform(const PluginDebugFloatHistory* history,
	                                           const PluginDebugTransform* drawTransform,
	                                           const PluginDebugVector* drawSize,
	                                           const PluginDebugDrawStyle* style)
	{
		DebugDrawFloatHistoryDeferred(history, ToTransform(drawTransform), ToVec(drawSize),
		                              ToStyle(style), /*bUseTransform*/ true);
	}

	static void DebugDrawFloatHistoryLocation(const PluginDebugFloatHistory* history,
	                                          const PluginDebugVector* drawLocation,
	                                          const PluginDebugVector* drawSize,
	                                          const PluginDebugDrawStyle* style)
	{
		DD::DTransform t{};
		t.location = ToVec(drawLocation);
		t.scale    = DD::DVec{ 1.0, 1.0, 1.0 };

		DebugDrawFloatHistoryDeferred(history, t, ToVec(drawSize),
		                              ToStyle(style), /*bUseTransform*/ false);
	}

	static void DebugDrawFlushPersistentLines()
	{
		OnGameThread([]() { DD::FlushPersistentLines(); });
	}

	static void DebugDrawClearAllStrings()
	{
		OnGameThread([]() { DD::ClearAllStrings(); });
	}

	// Debug draw sub-interface struct (v50)
	static IPluginDebugDraw g_debugDraw = {
		DebugDrawIsAvailable,
		DebugDrawLine,
		DebugDrawPoint,
		DebugDrawCircle,
		DebugDrawSphere,
		DebugDrawBox,
		DebugDrawCapsule,
		DebugDrawCylinder,
		DebugDrawConeInDegrees,
		DebugDrawArrow,
		DebugDrawCoordinateSystem,
		DebugDrawPlane,
		DebugDrawFrustum,
		DebugDrawCamera,
		DebugDrawCameraAt,
		DebugDrawString,
		DebugDrawFloatHistoryTransform,
		DebugDrawFloatHistoryLocation,
		DebugDrawFlushPersistentLines,
		DebugDrawClearAllStrings
	};

	// HUD sub-interface struct (v16)
	static IPluginHUDEvents g_hudEvents = {
		HooksRegisterOnPostRender,
		HooksUnregisterOnPostRender,
		HooksGetGatherPlayersDataAddress,
		&g_debugDraw          // v50 -- appended, do not relocate
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
#else
		nullptr,        // HUDPostRender — null on server/generic builds
#endif
		// ClientMessageExec — always null since the ClientSaveStringToTxt transport
		// was removed. Slot kept so the struct layout never shifts under old plugins.
		nullptr,
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
		&g_objectWalker,     // v47 — appended at end to preserve layout for v44-46 plugins
		&g_delegateHook,     // v47 — appended at end, do not relocate
		&g_objectProperties, // v47 — appended at end, do not relocate
		nullptr,             // v63 -- Console; filled in below by GetPluginHooks()
	};
	static bool g_networkChannelInitialized = false;

	IPluginHooks* GetPluginHooks()
	{
		// ModConsole command registration + output sinks. Available on every
		// build: registering a command does not depend on either console
		// front-end being open.
		if (!g_pluginHooks.Console)
			g_pluginHooks.Console = PluginConsole::GetInterface();

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
