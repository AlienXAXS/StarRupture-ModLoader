#include "plugin_hooks.h"
#include "plugin_logger.h"

namespace PluginHooks
{
	static IPluginHooks* g_hooks = nullptr;

	void Initialize(IPluginHooks* hooks)
	{
		g_hooks = hooks;

		if (g_hooks)
		{
			PluginLogger::Debug("Hooks interface initialized");
		}
		else
		{
			PluginLogger::Error("Hooks interface is NULL!");
		}
	}

	HookHandle InstallHook(uintptr_t targetAddress, void* detourFunction, void** originalFunction)
	{
		if (!g_hooks || !g_hooks->InstallHook)
		{
			PluginLogger::Error("Hooks not initialized or InstallHook not available");
			return nullptr;
		}

		return g_hooks->InstallHook(targetAddress, detourFunction, originalFunction);
	}

	void RemoveHook(HookHandle handle)
	{
		if (!g_hooks || !g_hooks->RemoveHook)
		{
			PluginLogger::Error("Hooks not initialized or RemoveHook not available");
			return;
		}

		g_hooks->RemoveHook(handle);
	}

	bool IsHookInstalled(HookHandle handle)
	{
		if (!g_hooks || !g_hooks->IsHookInstalled)
		{
			return false;
		}

		return g_hooks->IsHookInstalled(handle);
	}

	bool PatchMemory(uintptr_t address, const uint8_t* data, size_t size)
	{
		if (!g_hooks || !g_hooks->PatchMemory)
		{
			PluginLogger::Error("Hooks not initialized or PatchMemory not available");
			return false;
		}

		return g_hooks->PatchMemory(address, data, size);
	}

	bool NopMemory(uintptr_t address, size_t size)
	{
		if (!g_hooks || !g_hooks->NopMemory)
		{
			PluginLogger::Error("Hooks not initialized or NopMemory not available");
			return false;
		}

		return g_hooks->NopMemory(address, size);
	}

	bool ReadMemory(uintptr_t address, void* buffer, size_t size)
	{
		if (!g_hooks || !g_hooks->ReadMemory)
		{
			PluginLogger::Error("Hooks not initialized or ReadMemory not available");
			return false;
		}

		return g_hooks->ReadMemory(address, buffer, size);
	}

	void RegisterWorldBeginPlayCallback(void (*callback)(SDK::UWorld*))
	{
		if (!g_hooks || !g_hooks->RegisterWorldBeginPlayCallback)
		{
			PluginLogger::Error("Hooks not initialized or RegisterWorldBeginPlayCallback not available");
			return;
		}

		g_hooks->RegisterWorldBeginPlayCallback(callback);
	}

	void UnregisterWorldBeginPlayCallback(void (*callback)(SDK::UWorld*))
	{
		if (!g_hooks || !g_hooks->UnregisterWorldBeginPlayCallback)
		{
			PluginLogger::Error("Hooks not initialized or UnregisterWorldBeginPlayCallback not available");
			return;
		}

		g_hooks->UnregisterWorldBeginPlayCallback(callback);
	}
}
