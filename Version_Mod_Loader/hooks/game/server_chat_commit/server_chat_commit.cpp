#include "pch.h"
#ifdef MODLOADER_SERVER_BUILD

#include "server_chat_commit.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"
#include "CoreUObject_classes.hpp"
#include <hooks/hooks_common.h>

// ---------------------------------------------------------------------------
// ServerExecuteConsoleCommand hook  (server builds only)
//
// ACrPlayerControllerBase::ServerExecuteConsoleCommand is a NetServer RPC
// (client calls it, server executes it) with a single FString Command param.
//
// We repurpose it as a silent Client->Server data channel.  When a command
// starts with "[MOD:" it is a mod envelope:
//   - Dispatch to NetworkChannel::DispatchServerMessage
//   - Do NOT call the original (suppress console command execution)
// All other commands pass straight through to the real handler.
//
// Hooking ExecFunction directly catches both:
//   - UObject::ProcessEvent path (local game code calling it)
//   - FObjectReplicator::ReceivedRPC path (actual network delivery)
//
// Params layout (Chimera_parameters.hpp CrPlayerControllerBase_ServerExecuteConsoleCommand):
//   +0x00  FString Command  { wchar_t* Data, int32 Num, int32 Max }
//   Total: 0x10 bytes
// ---------------------------------------------------------------------------

namespace Hooks::ServerChatCommit
{
    // exec signature: (UObject* Context, FFrame& Stack, void* Result)
    using ExecFunc_t = void(__fastcall*)(void* context, void* stack, void* result);

    // FFrame::Locals is at offset 0x20 in UE5 (vptr + Node + Object + Code)
    static constexpr size_t kFFrameLocalsOffset = 0x20;

    // Mod envelope prefix used by network_channel.cpp
    static constexpr wchar_t  kModPrefix[]    = L"[MOD:";
    static constexpr int32_t  kModPrefixChars = 5;

    static Hook            g_hook;
    static ExecFunc_t      g_origExec           = nullptr;
    static SDK::UFunction* g_serverExecCmdFunc   = nullptr;

    static void __fastcall ExecHook(void* context, void* stack, void* result)
    {
        void* locals = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + kFFrameLocalsOffset);

        if (locals)
        {
            struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };
            auto* cmd = reinterpret_cast<FStringView*>(locals);

            if (cmd->Data && cmd->Num >= kModPrefixChars + 1 &&
                wcsncmp(cmd->Data, kModPrefix, kModPrefixChars) == 0)
            {
                ModLoaderLogger::LogInfo(
                    L"[ServerChatCommit] Mod envelope received from context=%p Num=%d",
                    context, cmd->Num);

                // context is the APlayerController that sent the RPC
                bool consumed = NetworkChannel::DispatchServerMessage(context, cmd->Data, cmd->Num);
                if (consumed)
                    return; // suppress original -- don't execute as a console command
            }
        }

        if (g_origExec)
            g_origExec(context, stack, result);
    }

    bool Install()
    {
        if (g_hook.installed) return true;

        g_serverExecCmdFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ServerExecuteConsoleCommand", SDK::EClassCastFlags::Function);

        if (!g_serverExecCmdFunc)
        {
            ModLoaderLogger::LogError(
                L"[ServerChatCommit] ServerExecuteConsoleCommand UFunction not found in GObjects");
            return false;
        }

        auto execAddr = reinterpret_cast<uintptr_t>(g_serverExecCmdFunc->ExecFunction);
        if (!execAddr)
        {
            ModLoaderLogger::LogError(L"[ServerChatCommit] ExecFunction pointer is null");
            return false;
        }

        HMODULE mainMod = GetModuleHandleW(nullptr);
        auto base = reinterpret_cast<uintptr_t>(mainMod);
        ModLoaderLogger::LogInfo(
            L"[ServerChatCommit] Hooking ServerExecuteConsoleCommand ExecFunction at 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(execAddr),
            static_cast<unsigned long long>(execAddr - base));

        bool ok = g_hook.Install(
            execAddr,
            reinterpret_cast<void*>(&ExecHook),
            reinterpret_cast<void**>(&g_origExec));

        if (ok)
            ModLoaderLogger::LogInfo(L"[ServerChatCommit] ExecFunction hook installed successfully");
        else
            ModLoaderLogger::LogError(L"[ServerChatCommit] ExecFunction hook installation failed");

        return ok;
    }

    void Remove()
    {
        g_hook.Remove();
        g_origExec         = nullptr;
        g_serverExecCmdFunc = nullptr;
        ModLoaderLogger::LogInfo(L"[ServerChatCommit] ExecFunction hook removed");
    }

    bool IsInstalled()
    {
        return g_hook.installed;
    }

} // namespace Hooks::ServerChatCommit

#endif // MODLOADER_SERVER_BUILD
