#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD

#include "client_message.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"
#include "CoreUObject_classes.hpp"
#include "hooks/game/game_instance_init/game_instance_init.h"

// ---------------------------------------------------------------------------
// Why ExecFunction and not ProcessEvent:
//
// Incoming "Client" RPCs from the server are received by FObjectReplicator::
// ReceivedRPC, which builds an FFrame and calls UFunction::ExecFunction
// DIRECTLY -- it never goes through UObject::ProcessEvent.  Hooking
// ProcessEvent therefore misses all network-delivered packets.
//
// Hooking ExecFunction on the resolved ClientMessage UFunction intercepts
// both paths:
//   - Normal local call:  ProcessEvent -> FFrame -> ExecFunction (hooked)
//   - Network RPC path:   FObjectReplicator::ReceivedRPC -> FFrame -> ExecFunction (hooked)
//
// FFrame::Locals (offset 0x20) is the raw parameter buffer in both paths.
// ---------------------------------------------------------------------------

namespace Hooks::ClientMessage
{
    // exec signature: (UObject* Context, FFrame& Stack, void* Result)
    using ExecFunc_t = void(__fastcall*)(void* context, void* stack, void* result);

    // FFrame::Locals is at offset 0x20 in UE5 (after vptr + Node + Object + Code)
    static constexpr size_t kFFrameLocalsOffset = 0x20;

    static Hook            g_hook;
    static ExecFunc_t      g_origExec      = nullptr;
    static SDK::UFunction* g_clientMsgFunc = nullptr;

    // ProcessEvent observer -- fires for every ProcessEvent call; used as a
    // diagnostic alongside the ExecFunction hook to determine which path the
    // client-side RPC actually takes.
    static void ProcessEventObserver(void* /*obj*/, void* fn, void* params)
    {
        if (fn != g_clientMsgFunc) return;
        ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent observer fired: fn=%p params=%p", fn, params);
        if (params)
        {
            struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };
            auto* s = reinterpret_cast<FStringView*>(params);
            ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent params: Data=%p Num=%d", s->Data, s->Num);
            if (s->Data && s->Num > 0)
                NetworkChannel::DispatchClientMessage(s->Data, s->Num);
        }
    }

    static void __fastcall ExecHook(void* context, void* stack, void* result)
    {
        // Read FFrame::Locals -- the raw parameter buffer
        void* locals = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + kFFrameLocalsOffset);

        ModLoaderLogger::LogInfo(L"[ClientMessage] ExecHook fired: context=%p locals=%p", context, locals);

        if (locals)
        {
            // Layout matches ClientMsgParms on the send side:
            //   +0x00 FString S  { wchar_t* Data, int32 Num, int32 Max }
            struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };
            auto* s = reinterpret_cast<FStringView*>(locals);
            ModLoaderLogger::LogDebug(L"[ClientMessage] Params: Data=%p Num=%d", s->Data, s->Num);
            if (s->Data && s->Num > 0)
                NetworkChannel::DispatchClientMessage(s->Data, s->Num);
        }

        if (g_origExec)
            g_origExec(context, stack, result);
    }

    bool Install()
    {
        if (g_hook.installed) return true;

        // GObjects is ready at this point (called after first ProcessEvent / PluginInit)
        g_clientMsgFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ClientMessage", SDK::EClassCastFlags::Function);

        if (!g_clientMsgFunc)
        {
            ModLoaderLogger::LogError(L"[ClientMessage] ClientMessage UFunction not found in GObjects");
            return false;
        }
        ModLoaderLogger::LogInfo(L"[ClientMessage] ClientMessage UFunction at %p",
                                  static_cast<void*>(g_clientMsgFunc));

        auto execAddr = reinterpret_cast<uintptr_t>(g_clientMsgFunc->ExecFunction);
        if (!execAddr)
        {
            ModLoaderLogger::LogError(L"[ClientMessage] ExecFunction pointer is null");
            return false;
        }

        HMODULE mainMod = GetModuleHandleW(nullptr);
        auto base = reinterpret_cast<uintptr_t>(mainMod);
        ModLoaderLogger::LogInfo(L"[ClientMessage] Hooking ExecFunction at 0x%llX (base+0x%llX)",
                                  static_cast<unsigned long long>(execAddr),
                                  static_cast<unsigned long long>(execAddr - base));

        bool ok = g_hook.Install(
            execAddr,
            reinterpret_cast<void*>(&ExecHook),
            reinterpret_cast<void**>(&g_origExec));

        if (ok)
            ModLoaderLogger::LogInfo(L"[ClientMessage] ExecFunction hook installed successfully");
        else
            ModLoaderLogger::LogError(L"[ClientMessage] ExecFunction hook installation failed");

        // Also register a ProcessEvent observer as a parallel diagnostic.
        // If ProcessEvent fires but ExecFunction doesn't, the RPC takes a different path.
        Hooks::GameInstanceInit::RegisterProcessEventCallback(&ProcessEventObserver);
        ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent observer registered");

        return ok;
    }

    void Remove()
    {
        g_hook.Remove();
        g_origExec      = nullptr;
        g_clientMsgFunc = nullptr;
        ModLoaderLogger::LogInfo(L"[ClientMessage] ExecFunction hook removed");
    }

    bool IsInstalled()
    {
        return g_hook.installed;
    }

} // namespace Hooks::ClientMessage

#endif // MODLOADER_CLIENT_BUILD
