#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "hud_post_render.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::HUDPostRender
{
	// void AHUD::PostRender()
	using AHUD_PostRender_t = void(__fastcall*)(void* thisPtr);

	static Hook              g_hook;
	static AHUD_PostRender_t g_original            = nullptr;
	static uintptr_t         g_gatherPlayersDataAddr = 0;

	static std::vector<PluginHUDPostRenderCallback> g_pluginCallbacks;

	// ---------------------------------------------------------------------------
	// Detour
	// ---------------------------------------------------------------------------

	static void __fastcall Detour(void* thisPtr)
	{
		// Call the original first so the engine draws its own HUD before plugins
		if (g_original)
			g_original(thisPtr);

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
				ModLoaderLogger::LogError(L"[HUDPostRender] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[HUDPostRender] Unknown exception in callback");
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Install / Remove
	// ---------------------------------------------------------------------------

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[HUDPostRender] Installing hook...");

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		// --- AHUD::PostRender ---
		uintptr_t addr = Scanner::FindPatternInMainModule("AHUD_PostRender", ScanPatterns::AHUD_PostRender);
		if (!addr)
		{
			ModLoaderLogger::LogError(L"[HUDPostRender] AHUD::PostRender pattern not found");
			return false;
		}

		ModLoaderLogger::LogDebug(L"[HUDPostRender] AHUD::PostRender at 0x%llX (base+0x%llX)",
		                          static_cast<unsigned long long>(addr),
		                          static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[HUDPostRender] AHUD::PostRender hook installed successfully");
		else
		{
			ModLoaderLogger::LogError(L"[HUDPostRender] AHUD::PostRender hook installation failed");
			return false;
		}

		// --- GatherPlayersData (optional, cached for plugins to use) ---
		g_gatherPlayersDataAddr = Scanner::FindPatternInMainModule(
			"GatherPlayersData", ScanPatterns::GatherPlayersData);

		if (g_gatherPlayersDataAddr)
			ModLoaderLogger::LogInfo(L"[HUDPostRender] GatherPlayersData at 0x%llX (base+0x%llX)",
			                         static_cast<unsigned long long>(g_gatherPlayersDataAddr),
			                         static_cast<unsigned long long>(g_gatherPlayersDataAddr - base));
		else
			ModLoaderLogger::LogWarn(L"[HUDPostRender] GatherPlayersData pattern not found -- "
			                         L"player markers may not update in real time");

		return true;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[HUDPostRender] Removing hook...");
		g_hook.Remove();
		g_original = nullptr;
		g_gatherPlayersDataAddr = 0;
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	// ---------------------------------------------------------------------------
	// Callback registration
	// ---------------------------------------------------------------------------

	void RegisterPluginCallback(PluginHUDPostRenderCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[HUDPostRender] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[HUDPostRender] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[HUDPostRender] Failed to install hook for HUD post-render callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[HUDPostRender] Plugin callback registered (%zu total)",
		                          g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginHUDPostRenderCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[HUDPostRender] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	uintptr_t GetGatherPlayersDataAddress()
	{
		return g_gatherPlayersDataAddr;
	}
}

#endif // MODLOADER_CLIENT_BUILD
