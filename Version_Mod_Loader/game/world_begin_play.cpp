#include "pch.h"
#include "world_begin_play.h"
#include "../logger.h"
#include "../scanner.h"
#include <vector>
#include <algorithm>

// Forward declarations for SDK types we use
namespace SDK
{
    class UObject
    {
    public:
        std::string GetName() const;
      std::string GetFullName() const;
    };

 class UGameInstance
    {
    public:
        std::string GetName() const;
    };

    template<typename T>
 class TArray
    {
    public:
        int Num() const;
    };

    class UNetConnection;

    class UNetDriver
    {
    public:
 std::string GetName() const;
        TArray<UNetConnection*> ClientConnections;
    };

    class UWorld
    {
    public:
   std::string GetName() const;
        std::string GetFullName() const;
        UGameInstance* OwningGameInstance;
        UNetDriver* NetDriver;
    };
}

namespace Hooks::WorldBeginPlay
{
	// UCrSessionWorldLoaderSubsystem::OnWorldBeginPlay signature
	// Uses proper SDK types
	typedef void(__fastcall* OnWorldBeginPlay_t)(SDK::UObject* thisPtr, SDK::UWorld* inWorld);

	static Hook g_hook;
	static OnWorldBeginPlay_t g_original = nullptr;
	static long g_callCount = 0;

	// Callback for plugins to receive world begin play events
	static std::vector<PluginWorldBeginPlayCallback> g_pluginCallbacks;

	static void __fastcall Detour(SDK::UObject* thisPtr, SDK::UWorld* inWorld)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		// Always log the world begin play event for debugging
		ModLoader::LogDebug(L"[WorldBeginPlay] World begin play detected (#%ld)", callNum);
		
		// Get world name for filtering
		std::string worldName;
		if (inWorld)
		{
			worldName = inWorld->GetName();
			ModLoader::LogDebug(L"[WorldBeginPlay]   World: %S", worldName.c_str());
		}

		// Check if this is the ChimeraMain world
		bool isChimeraWorld = worldName.find("ChimeraMain") != std::string::npos;
		
		if (!isChimeraWorld)
		{
			// Not the world we care about - just call original and return
			ModLoader::LogInfo(L"[WorldBeginPlay]   Skipping - not ChimeraMain world");
			if (g_original)
			{
				g_original(thisPtr, inWorld);
			}
			return;
		}

		// This is ChimeraMain - proceed with full logging and initialization
		ModLoader::LogInfo(L"[WorldBeginPlay] ChimeraMain world begin play detected (#%ld)", callNum);
		
		// Log using SDK types
		if (thisPtr)
		{
			ModLoader::LogDebug(L"[WorldBeginPlay]   Subsystem: %S (Class: %S)", 
				thisPtr->GetName().c_str(), 
				thisPtr->GetFullName().c_str());
		}

		if (inWorld)
		{
			ModLoader::LogDebug(L"[WorldBeginPlay]   World: %S (Class: %S)", 
				inWorld->GetName().c_str(),
				inWorld->GetFullName().c_str());
			
			// Log game instance if available
			if (inWorld->OwningGameInstance)
			{
				ModLoader::LogDebug(L"[WorldBeginPlay]   GameInstance: %S", 
					inWorld->OwningGameInstance->GetName().c_str());
			}
			
			// Log NetDriver if available (dedicated server)
			if (inWorld->NetDriver)
			{
				ModLoader::LogDebug(L"[WorldBeginPlay]   NetDriver: %S (Clients: %d)", 
					inWorld->NetDriver->GetName().c_str(),
					inWorld->NetDriver->ClientConnections.Num());
			}
		}

		// Call original
		ModLoader::LogDebug(L"[WorldBeginPlay]   Calling original OnWorldBeginPlay...");
		if (g_original)
		{
			g_original(thisPtr, inWorld);
			ModLoader::LogDebug(L"[WorldBeginPlay]   Original returned");
		}
		else
		{
			ModLoader::LogError(L"[WorldBeginPlay] Original function pointer is null!");
		}

		// Notify all registered plugins (with error isolation)
		if (!g_pluginCallbacks.empty())
		{
			ModLoader::LogDebug(L"[WorldBeginPlay] Notifying %zu plugin(s)...", g_pluginCallbacks.size());
			
			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				__try
				{
					ModLoader::LogTrace(L"[WorldBeginPlay]   Calling plugin callback #%zu", i + 1);
					g_pluginCallbacks[i](inWorld);
					ModLoader::LogTrace(L"[WorldBeginPlay]   Plugin callback #%zu completed", i + 1);
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					ModLoader::LogError(L"[WorldBeginPlay] EXCEPTION in plugin callback #%zu (code: 0x%08lX)", 
						i + 1, GetExceptionCode());
					ModLoader::LogError(L"[WorldBeginPlay] Continuing to notify other plugins...");
				}
			}

			ModLoader::LogDebug(L"[WorldBeginPlay] All plugin callbacks completed");
		}

		ModLoader::LogDebug(L"[WorldBeginPlay] OnWorldBeginPlay complete (#%ld)", callNum);
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[WorldBeginPlay] Installing hook...");

		// Hard-coded pattern for UCrSessionWorldLoaderSubsystem::OnWorldBeginPlay
		const char* pattern = "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC ?? 0F 29 74 24 ?? 48 8B E9";

		ModLoader::LogInfo(L"[WorldBeginPlay] Scanning for OnWorldBeginPlay...");
		ModLoader::LogDebug(L"[WorldBeginPlay]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule(pattern);

		if (!addr)
		{
			ModLoader::LogError(L"[WorldBeginPlay] OnWorldBeginPlay pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoader::LogDebug(L"[WorldBeginPlay] OnWorldBeginPlay found at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[WorldBeginPlay] Hook installed successfully (filtering for ChimeraMain worlds)");
		else
			ModLoader::LogError(L"[WorldBeginPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[WorldBeginPlay] Removing hook...");
		g_hook.Remove();
		
		// Clear plugin callbacks
		g_pluginCallbacks.clear();
	}

	long GetCallCount()
	{
		return g_callCount;
	}

	void RegisterPluginCallback(PluginWorldBeginPlayCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[WorldBeginPlay] RegisterPluginCallback: null callback provided");
			return;
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[WorldBeginPlay] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginWorldBeginPlayCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[WorldBeginPlay] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
