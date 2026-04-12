#include "pch.h"
#include "game_instance_init.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "plugins/plugin_manager.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::GameInstanceInit
{
	using ProcessEvent_t = void(__fastcall*)(void* obj, void* fn, void* params);

	static Hook             g_hook;
	static ProcessEvent_t   g_original  = nullptr;
	static volatile LONG    g_initFired = 0;

	static std::vector<ProcessEventCallback> g_callbacks;

	static void __fastcall Detour(void* obj, void* fn, void* params)
	{
		// One-shot: only the very first ProcessEvent call triggers plugin init.
		if (InterlockedCompareExchange(&g_initFired, 1, 0) == 0)
		{
			ModLoaderLogger::LogInfo(L"[GameInstanceInit] First ProcessEvent -- GObjects ready, initializing plugins");
			PluginManager::InitAllLoadedPlugins();
		}

		// Dispatch to registered observers before calling original.
		for (auto* cb : g_callbacks)
			if (cb) cb(obj, fn, params);

		if (g_original)
			g_original(obj, fn, params);
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[GameInstanceInit] Installing UObject::ProcessEvent hook...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"UObject::ProcessEvent", ScanPatterns::UObject_ProcessEvent);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[GameInstanceInit] UObject::ProcessEvent pattern not found");
			return false;
		}

		HMODULE mainMod = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainMod);
		ModLoaderLogger::LogDebug(L"[GameInstanceInit] UObject::ProcessEvent at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool ok = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (ok)
			ModLoaderLogger::LogInfo(L"[GameInstanceInit] Hook installed -- will init plugins on first ProcessEvent");
		else
			ModLoaderLogger::LogError(L"[GameInstanceInit] Hook installation failed");

		return ok;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[GameInstanceInit] Removing hook...");
		g_hook.Remove();
		g_original  = nullptr;
		g_initFired = 0;
		g_callbacks.clear();
	}

	void RegisterProcessEventCallback(ProcessEventCallback cb)
	{
		if (cb)
			g_callbacks.push_back(cb);
	}

	void UnregisterProcessEventCallback(ProcessEventCallback cb)
	{
		g_callbacks.erase(std::remove(g_callbacks.begin(), g_callbacks.end(), cb), g_callbacks.end());
	}
}
