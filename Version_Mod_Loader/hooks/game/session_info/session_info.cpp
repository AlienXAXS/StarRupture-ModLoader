#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD

#include "session_info.h"
#include "logging/logger.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

// UCrSessionSubsystem field offsets confirmed via IDA Pro.
// CommonSessionOnlineMode : uint8  @ 0x006B  (Offline=0, LAN=1, Online=2)
// bIsServer               : bool   @ 0x006D
static constexpr size_t k_CommonSessionOnlineModeOffset = 0x006B;
static constexpr size_t k_bIsServerOffset               = 0x006D;

namespace Hooks::SessionInfo
{
    static SDK::UCrSessionSubsystem* GetSubsystem()
    {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
            return nullptr;

        auto* subsystem = static_cast<SDK::UCrSessionSubsystem*>(
            SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(
                world,
                SDK::UCrSessionSubsystem::StaticClass()));

        return subsystem;
    }

    EPluginSessionOnlineMode GetSessionOnlineMode()
    {
        const SDK::UCrSessionSubsystem* subsystem = GetSubsystem();
        if (!subsystem)
            return EPluginSessionOnlineMode::Unknown;

        const uint8_t raw = *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(subsystem) + k_CommonSessionOnlineModeOffset);

        switch (raw)
        {
        case 0:  return EPluginSessionOnlineMode::Offline;
        case 1:  return EPluginSessionOnlineMode::LAN;
        case 2:  return EPluginSessionOnlineMode::Online;
        default: return EPluginSessionOnlineMode::Unknown;
        }
    }

    bool IsMultiplayer()
    {
        const EPluginSessionOnlineMode mode = GetSessionOnlineMode();
        return mode == EPluginSessionOnlineMode::LAN || mode == EPluginSessionOnlineMode::Online;
    }

    bool IsServer()
    {
        const SDK::UCrSessionSubsystem* subsystem = GetSubsystem();
        if (!subsystem)
            return false;

        return *reinterpret_cast<const bool*>(
            reinterpret_cast<const uint8_t*>(subsystem) + k_bIsServerOffset);
    }
}

#endif // MODLOADER_CLIENT_BUILD
