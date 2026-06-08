#include "pch.h"
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

#include "session_info.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "Engine_classes.hpp"

namespace Hooks::SessionInfo
{
    // ENetMode __fastcall AActor::InternalGetNetMode(AActor* this)
    // AActor is reflected in the SDK, so we can pass SDK::AActor* directly.
    using AActor_InternalGetNetMode_t = int(__fastcall*)(SDK::AActor* This);

    static AActor_InternalGetNetMode_t g_original = nullptr;

    bool Install()
    {
        ModLoaderLogger::LogInfo(L"[SessionInfo] Scanning for AActor::InternalGetNetMode...");

        uintptr_t addr = Scanner::FindPatternInMainModule(
            "AActor::InternalGetNetMode", ScanPatterns::AActor_InternalGetNetMode);
        if (!addr)
        {
            ModLoaderLogger::LogError(L"[SessionInfo] Failed to find AActor::InternalGetNetMode pattern");
            return false;
        }

        g_original = reinterpret_cast<AActor_InternalGetNetMode_t>(addr);
        ModLoaderLogger::LogInfo(L"[SessionInfo] Resolved AActor::InternalGetNetMode at 0x%llX", addr);
        return true;
    }

    // Any actor will do -- InternalGetNetMode resolves the net mode from the
    // actor's owning world/level. The game state is reliably present once a
    // world has begun play.
    static SDK::AActor* GetAnyActor()
    {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
            return nullptr;

        return reinterpret_cast<SDK::AActor*>(world->GameState);
    }

    EPluginNetMode GetNetMode()
    {
        if (!g_original)
            return EPluginNetMode::Unknown;

        SDK::AActor* actor = GetAnyActor();
        if (!actor)
            return EPluginNetMode::Unknown;

        const int raw = g_original(actor);
        switch (raw)
        {
        case 0:  return EPluginNetMode::Standalone;
        case 1:  return EPluginNetMode::DedicatedServer;
        case 2:  return EPluginNetMode::ListenServer;
        case 3:  return EPluginNetMode::Client;
        default: return EPluginNetMode::Unknown;
        }
    }

    bool IsMultiplayer()
    {
        const EPluginNetMode mode = GetNetMode();
        return mode == EPluginNetMode::ListenServer || mode == EPluginNetMode::Client;
    }

    bool IsServer()
    {
        const EPluginNetMode mode = GetNetMode();
        return mode == EPluginNetMode::DedicatedServer || mode == EPluginNetMode::ListenServer;
    }
}

#endif // MODLOADER_SERVER_BUILD || MODLOADER_CLIENT_BUILD
