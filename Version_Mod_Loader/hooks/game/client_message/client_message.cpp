#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD

#include "client_message.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"

// SDK headers for UObject, UFunction, Offsets, and InSDKUtils::GetImageBase()
#include "CoreUObject_classes.hpp"

namespace Hooks::ClientMessage
{
    // ProcessEvent signature: (UObject* self, UFunction* func, void* parms)
    using ProcessEvent_t = void(__fastcall*)(SDK::UObject* self,
                                             SDK::UFunction* func,
                                             void* parms);

    static Hook           g_hook;
    static ProcessEvent_t g_original = nullptr;

    // Cached pointer to the ClientMessage UFunction.  Resolved lazily on first
    // ProcessEvent call that has a non-null UFunction.
    static SDK::UFunction* g_clientMsgFunc = nullptr;

    // Try to resolve the ClientMessage UFunction from GObjects.
    // Uses FindObjectFast which searches by short name -- there is only one
    // UFunction named "ClientMessage" in the PlayerController class hierarchy.
    // Returns true if newly resolved.
    static bool TryResolveClientMsgFunc()
    {
        if (g_clientMsgFunc) return false; // already cached

        g_clientMsgFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ClientMessage", SDK::EClassCastFlags::Function);

        if (g_clientMsgFunc)
            ModLoaderLogger::LogInfo(
                L"[ClientMessage] ClientMessage UFunction resolved at %p",
                static_cast<void*>(g_clientMsgFunc));

        return g_clientMsgFunc != nullptr;
    }

    // Detour for UObject::ProcessEvent
    static void __fastcall Detour(SDK::UObject* self, SDK::UFunction* func, void* parms)
    {
        // Lazily resolve the UFunction we are listening for -- only once, O(GObjects) cost
        if (!g_clientMsgFunc)
            TryResolveClientMsgFunc();

        // Fast-path: only act on ClientMessage calls
        if (func && func == g_clientMsgFunc)
        {
            // Params layout (matches Engine_parameters.hpp Params::PlayerController_ClientMessage):
            //   +0x00  FString S  = { wchar_t* Data (+0), int32 Num (+8), int32 Max (+0x0C) }
            //   +0x10  FName Type = { int32 CompIdx (+0x10), uint32 Number (+0x14) }
            //   +0x18  float MsgLifeTime
            //   +0x1C  uint8 pad[4]
            // We only need the wchar_t* Data pointer and NumElements from FString.
            struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };
            auto* s = reinterpret_cast<FStringView*>(parms);
            if (s->Data && s->Num > 0)
                NetworkChannel::DispatchClientMessage(s->Data, s->Num);
        }

        if (g_original)
            g_original(self, func, parms);
    }

    bool Install()
    {
        if (g_hook.installed) return true;

        HMODULE mainModule = GetModuleHandleW(nullptr);
        auto base = reinterpret_cast<uintptr_t>(mainModule);

        // ProcessEvent is a well-known function at a fixed image offset.
        // The address comes from the client SDK's Offsets::ProcessEvent.
        uintptr_t addr = SDK::InSDKUtils::GetImageBase() + SDK::Offsets::ProcessEvent;

        ModLoaderLogger::LogInfo(L"[ClientMessage] Installing ProcessEvent hook at 0x%llX (base+0x%llX)",
                                  static_cast<unsigned long long>(addr),
                                  static_cast<unsigned long long>(addr - base));

        bool ok = g_hook.Install(addr,
                                 reinterpret_cast<void*>(&Detour),
                                 reinterpret_cast<void**>(&g_original));

        if (ok)
            ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent hook installed");
        else
            ModLoaderLogger::LogError(L"[ClientMessage] ProcessEvent hook installation failed");

        return ok;
    }

    void Remove()
    {
        g_hook.Remove();
        g_original       = nullptr;
        g_clientMsgFunc  = nullptr;
        ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent hook removed");
    }

    bool IsInstalled()
    {
        return g_hook.installed;
    }

} // namespace Hooks::ClientMessage

#endif // MODLOADER_CLIENT_BUILD
