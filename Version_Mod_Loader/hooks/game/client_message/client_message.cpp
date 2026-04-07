#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD

#include "client_message.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"
#include "CoreUObject_classes.hpp"
#include "hooks/game/game_instance_init/game_instance_init.h"

// ---------------------------------------------------------------------------
// ClientSaveStringToTxt hook  (client builds only)
//
// ACrPlayerControllerBase::ClientSaveStringToTxt is a game debug RPC
// (Net, NetReliable, NetClient) that the server sends to the client.
//
// We repurpose it as a silent data channel by using a "MOD_NET" sentinel
// in the Path parameter.  When the sentinel is detected:
//   - Dispatch InString to NetworkChannel (envelope decode + plugin handlers)
//   - Do NOT call the original exec (suppress the file-write side effect)
// Non-mod calls pass straight through to the original.
//
// Two interception paths cover all receive scenarios:
//   ProcessEvent observer: fires when the game calls via UObject::ProcessEvent.
//   ExecFunction hook:     fires when the network layer calls ExecFunction
//                          directly (FObjectReplicator::ReceivedRPC path).
//
// Params layout (Chimera_parameters.hpp CrPlayerControllerBase_ClientSaveStringToTxt):
//   +0x00  FString InString  { wchar_t* Data, int32 Num, int32 Max }
//   +0x10  FString Path      { wchar_t* Data, int32 Num, int32 Max }
//   Total: 0x20 bytes
// ---------------------------------------------------------------------------

namespace Hooks::ClientMessage
{
    // exec signature: (UObject* Context, FFrame& Stack, void* Result)
    using ExecFunc_t = void(__fastcall*)(void* context, void* stack, void* result);

    // FFrame::Locals is at offset 0x20 in UE5 (vptr + Node + Object + Code)
    static constexpr size_t kFFrameLocalsOffset = 0x20;

    // Sentinel string written into the Path parameter to tag mod traffic.
    // Must match the value used by network_channel.cpp on the server send path.
    static constexpr wchar_t  kSentinel[]    = L"MOD_NET";
    static constexpr int32_t  kSentinelChars = 7; // not counting null terminator

    static Hook            g_hook;
    static ExecFunc_t      g_origExec         = nullptr;
    static SDK::UFunction* g_clientSaveTxtFunc = nullptr;

    struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };

    // Returns true if the Path FString matches the sentinel "MOD_NET".
    static bool IsModSentinel(const FStringView* path)
    {
        // Num includes null terminator, so "MOD_NET\0" → Num == 8
        return path && path->Data && path->Num >= kSentinelChars + 1 &&
               wcsncmp(path->Data, kSentinel, kSentinelChars) == 0;
    }

    // ProcessEvent observer -- fires for every ProcessEvent call.
    // Params here is a raw CrPlayerControllerBase_ClientSaveStringToTxt struct.
    static void ProcessEventObserver(void* /*obj*/, void* fn, void* params)
    {
        if (fn != g_clientSaveTxtFunc) return;
        if (!params) return;

        auto* p = reinterpret_cast<FStringView*>(params); // InString at [0], Path at [1]
        if (!IsModSentinel(&p[1])) return;

        ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent observer fired (mod traffic): Num=%d", p[0].Num);
        if (p[0].Data && p[0].Num > 0)
            NetworkChannel::DispatchClientMessage(p[0].Data, p[0].Num);
        // Do NOT call original -- suppress the file-write side effect
    }

    static void __fastcall ExecHook(void* context, void* stack, void* result)
    {
        // Read FFrame::Locals -- the raw parameter struct
        void* locals = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + kFFrameLocalsOffset);

        ModLoaderLogger::LogInfo(L"[ClientMessage] ExecHook fired: context=%p locals=%p", context, locals);

        if (locals)
        {
            auto* p = reinterpret_cast<FStringView*>(locals); // InString at [0], Path at [1]

            if (IsModSentinel(&p[1]))
            {
                ModLoaderLogger::LogInfo(L"[ClientMessage] Mod sentinel detected: InStr.Num=%d", p[0].Num);
                if (p[0].Data && p[0].Num > 0)
                    NetworkChannel::DispatchClientMessage(p[0].Data, p[0].Num);
                // Suppress original -- no file write
                if (g_origExec)
                    g_origExec(context, stack, result);
                return;
            }

            ModLoaderLogger::LogDebug(L"[ClientMessage] Non-mod ExecHook: Path.Num=%d -- passing through",
                                      p[1].Num);
        }

        if (g_origExec)
            g_origExec(context, stack, result);
    }

    bool Install()
    {
        if (g_hook.installed) return true;

        g_clientSaveTxtFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ClientSaveStringToTxt", SDK::EClassCastFlags::Function);

        if (!g_clientSaveTxtFunc)
        {
            ModLoaderLogger::LogError(L"[ClientMessage] ClientSaveStringToTxt UFunction not found in GObjects");
            return false;
        }
        ModLoaderLogger::LogInfo(L"[ClientMessage] ClientSaveStringToTxt UFunction at %p",
                                  static_cast<void*>(g_clientSaveTxtFunc));

        auto execAddr = reinterpret_cast<uintptr_t>(g_clientSaveTxtFunc->ExecFunction);
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

        // ProcessEvent observer as a parallel receive path
        Hooks::GameInstanceInit::RegisterProcessEventCallback(&ProcessEventObserver);
        ModLoaderLogger::LogInfo(L"[ClientMessage] ProcessEvent observer registered");

        return ok;
    }

    void Remove()
    {
        Hooks::GameInstanceInit::UnregisterProcessEventCallback(&ProcessEventObserver);
        g_hook.Remove();
        g_origExec          = nullptr;
        g_clientSaveTxtFunc = nullptr;
        ModLoaderLogger::LogInfo(L"[ClientMessage] Hooks removed");
    }

    bool IsInstalled()
    {
        return g_hook.installed;
    }

} // namespace Hooks::ClientMessage

#endif // MODLOADER_CLIENT_BUILD
