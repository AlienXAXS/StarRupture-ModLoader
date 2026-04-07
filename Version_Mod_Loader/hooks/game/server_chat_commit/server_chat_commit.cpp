#include "pch.h"
#ifdef MODLOADER_SERVER_BUILD

#include "server_chat_commit.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"

// SDK headers for UObject, UFunction, Offsets, and InSDKUtils::GetImageBase()
#include "CoreUObject_classes.hpp"
#include <hooks/hooks_common.h>

namespace Hooks::ServerChatCommit
{
    // ProcessEvent signature: (UObject* self, UFunction* func, void* parms)
    using ProcessEvent_t = void(__fastcall*)(SDK::UObject* self,
                                             SDK::UFunction* func,
                                             void* parms);

    static Hook           g_hook;
    static ProcessEvent_t g_original = nullptr;

    // Cached pointer to the ServerChatCommit UFunction.  Resolved lazily on first
    // ProcessEvent call that has a non-null UFunction.
    static SDK::UFunction* g_serverChatFunc = nullptr;

    // Try to resolve the ServerChatCommit UFunction from GObjects.
    // Uses FindObjectFast which searches by short name -- there is only one
    // UFunction named "ServerChatCommit" in the player controller class hierarchy.
    static bool TryResolveServerChatFunc()
    {
        if (g_serverChatFunc) return false; // already cached

        g_serverChatFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ServerChatCommit", SDK::EClassCastFlags::Function);

        if (g_serverChatFunc)
            ModLoaderLogger::LogInfo(
                L"[ServerChatCommit] ServerChatCommit UFunction resolved at %p",
                static_cast<void*>(g_serverChatFunc));

        return g_serverChatFunc != nullptr;
    }

    // Detour for UObject::ProcessEvent
    static void __fastcall Detour(SDK::UObject* self, SDK::UFunction* func, void* parms)
    {
        // Lazily resolve the UFunction we are listening for -- only once, O(GObjects) cost
        if (!g_serverChatFunc)
            TryResolveServerChatFunc();

        // Fast-path: only act on ServerChatCommit calls
        if (func && func == g_serverChatFunc)
        {
            // Params layout (matches Chimera_parameters.hpp CrPlayerControllerBase_ServerChatCommit):
            //   +0x00  FString Text = { wchar_t* Data (+0), int32 Num (+8), int32 Max (+0x0C) }
            // Total size: 0x10 bytes
            struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };
            auto* s = reinterpret_cast<FStringView*>(parms);
            if (s->Data && s->Num > 0)
            {
                // Dispatch first; if it is a mod envelope the channel will consume it
                // and return true, meaning we should NOT call the original (suppress chat).
                bool consumed = NetworkChannel::DispatchServerMessage(self, s->Data, s->Num);
                if (consumed)
                    return; // suppress original -- do not let mod traffic appear as chat
            }
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
        // The address comes from the server SDK's Offsets::ProcessEvent.
        uintptr_t addr = SDK::InSDKUtils::GetImageBase() + SDK::Offsets::ProcessEvent;

        ModLoaderLogger::LogInfo(L"[ServerChatCommit] Installing ProcessEvent hook at 0x%llX (base+0x%llX)",
                                  static_cast<unsigned long long>(addr),
                                  static_cast<unsigned long long>(addr - base));

        bool ok = g_hook.Install(addr,
                                 reinterpret_cast<void*>(&Detour),
                                 reinterpret_cast<void**>(&g_original));

        if (ok)
            ModLoaderLogger::LogInfo(L"[ServerChatCommit] ProcessEvent hook installed");
        else
            ModLoaderLogger::LogError(L"[ServerChatCommit] ProcessEvent hook installation failed");

        return ok;
    }

    void Remove()
    {
        g_hook.Remove();
        g_original       = nullptr;
        g_serverChatFunc = nullptr;
        ModLoaderLogger::LogInfo(L"[ServerChatCommit] ProcessEvent hook removed");
    }

    bool IsInstalled()
    {
        return g_hook.installed;
    }

} // namespace Hooks::ServerChatCommit

#endif // MODLOADER_SERVER_BUILD
