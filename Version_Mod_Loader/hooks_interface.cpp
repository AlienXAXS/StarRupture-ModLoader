#include "hooks_interface.h"
#include "hooks_common.h"
#include "logger.h"
#include "engine_allocator.h"
#include "game/world_begin_play/world_begin_play.h"
#include "game/engine_init/engine_init.h"
#include "game/engine_shutdown/engine_shutdown.h"
#include <unordered_map>
#include <mutex>

namespace ModLoader
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

	// Global hooks interface instance
	static IPluginHooks g_pluginHooks = {
		HooksInstallHook,
		HooksRemoveHook,
		HooksIsHookInstalled,
		HooksPatchMemory,
		HooksNopMemory,
		HooksReadMemory,
		HooksRegisterWorldBeginPlayCallback,
		HooksUnregisterWorldBeginPlayCallback,
		HooksRegisterEngineInitCallback,
		HooksUnregisterEngineInitCallback,
		HooksRegisterEngineShutdownCallback,
		HooksUnregisterEngineShutdownCallback,
		HooksEngineAlloc,
		HooksEngineFree,
		HooksIsEngineAllocatorAvailable
	};

	IPluginHooks* GetPluginHooks()
	{
		return &g_pluginHooks;
	}
}
