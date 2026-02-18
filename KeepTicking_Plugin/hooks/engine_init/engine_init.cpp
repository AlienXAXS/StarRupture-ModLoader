#include "engine_init.h"
#include "plugin_logger.h"
#include "plugin_scanner.h"

namespace Hooks::EngineInit
{
	static Hook g_hook;
	static FEngineLoop_Init_t g_original = nullptr;
	static bool g_engineInitialized = false;
	static void (*g_callback)() = nullptr;

	static int32_t __fastcall Detour(void* thisPtr)
	{
		PluginLogger::Info("Engine initialization detected");
		PluginLogger::Debug("  FEngineLoop=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original
		int32_t result = 0;
		if (g_original)
		{
			PluginLogger::Debug("  Calling original FEngineLoop::Init...");
			result = g_original(thisPtr);
			PluginLogger::Debug("  Original returned: %d", result);
		}
		else
		{
			PluginLogger::Error("FEngineLoop::Init: Original function pointer is null!");
		}

		// Mark as initialized and call callback
		if (!g_engineInitialized)
		{
			g_engineInitialized = true;

			if (g_callback)
			{
				PluginLogger::Debug("  Calling engine init callback...");
				g_callback();
			}
		}

		return result;
	}

	bool Install()
	{
		PluginLogger::Info("Installing engine initialization hook...");

		// Pattern for FEngineLoop::Init
		const char* pattern = 
			"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? 48 8D 15";

		PluginLogger::Info("Scanning for FEngineLoop::Init...");
		uintptr_t addr = PluginScanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			PluginLogger::Error("FEngineLoop::Init not found - mod cannot initialize");
			PluginLogger::Error("This is a CRITICAL error - mod will not function");
			return false;
		} 

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		PluginLogger::Info("✓ FEngineLoop::Init found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
		{
			PluginLogger::Info("Engine init hook installed - waiting for engine to be ready...");
		}
		else
		{
			PluginLogger::Error("FEngineLoop::Init hook installation failed - mod cannot function");
		}

		return hookOk;
	}

	void Remove()
	{
		PluginLogger::Info("Removing FEngineLoop::Init hook...");
		g_hook.Remove();
	}

	bool IsEngineInitialized()
	{
		return g_engineInitialized;
	}

	void SetEngineInitCallback(void (*callback)())
	{
		g_callback = callback;
	}
}
