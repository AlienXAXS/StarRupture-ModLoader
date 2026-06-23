#include "pch.h"
#include "crafting_finished.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "Chimera_classes.hpp"
#include <vector>
#include <algorithm>

namespace Hooks::CraftingFinished
{
	// ACrCrafter::NativeOnItemCraftingComplete(ACrCrafter* this, FMassEntityHandle EntityHandle, FName SignalName)
	// FMassEntityHandle and FName are both 8-byte PODs, passed by value in registers.
	using ACrCrafter_NativeOnItemCraftingComplete_t = void(__fastcall*)(void* thisPtr, uint64_t entityHandle, uint64_t signalName);

	static Hook g_hook;
	static ACrCrafter_NativeOnItemCraftingComplete_t g_original = nullptr;

	// Callback for plugins to receive crafting-finished events
	static std::vector<PluginCraftingFinishedCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr, uint64_t entityHandle, uint64_t signalName)
	{
		// Call original first so crafting-complete sounds play and the
		// ACrCrafter::OnItemCraftingComplete BlueprintEvent fires before we notify
		if (g_original)
		{
			g_original(thisPtr, entityHandle, signalName);
		}

		void* craftingComponent = nullptr;
		if (thisPtr)
		{
			auto* crafter = reinterpret_cast<SDK::ACrCrafter*>(thisPtr);
			craftingComponent = crafter->CraftComponent;
		}

		// FMassEntityHandle { int32 Index; int32 SerialNumber; } packed into the low/high 32 bits
		int32_t entityIndex = static_cast<int32_t>(entityHandle & 0xFFFFFFFFu);
		int32_t entitySerial = static_cast<int32_t>((entityHandle >> 32) & 0xFFFFFFFFu);

		// Notify registered plugins
		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (!g_pluginCallbacks[i])
				continue;

			try
			{
				g_pluginCallbacks[i](thisPtr, craftingComponent, entityIndex, entitySerial);
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[CraftingFinished] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[CraftingFinished] Unknown exception in callback");
			}
		}
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[CraftingFinished] Installing hook...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"ACrCrafter::NativeOnItemCraftingComplete",
			ScanPatterns::ACrCrafter_NativeOnItemCraftingComplete);
		if (!addr)
		{
			ModLoaderLogger::LogError(L"[CraftingFinished] Pattern scan failed -- hook not installed");
			return false;
		}

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[CraftingFinished] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[CraftingFinished] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[CraftingFinished] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginCraftingFinishedCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[CraftingFinished] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[CraftingFinished] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[CraftingFinished] Failed to install hook for crafting-finished callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[CraftingFinished] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginCraftingFinishedCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[CraftingFinished] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
