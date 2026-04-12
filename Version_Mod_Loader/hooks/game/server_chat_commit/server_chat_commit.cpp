#include "pch.h"
#ifdef MODLOADER_SERVER_BUILD

#include "server_chat_commit.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"
#include "CoreUObject_classes.hpp"
#include "hooks/game/game_instance_init/game_instance_init.h"
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

    struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };

    static bool TryDispatchEnvelope(void* senderContext, FStringView* cmd, bool allowNeuter)
    {
        if (!cmd || !cmd->Data || cmd->Num < kModPrefixChars + 1)
            return false;

        ModLoaderLogger::LogTrace(
            L"[ServerChatCommit] Command candidate data=%p num=%d max=%d sender=%p",
            cmd->Data,
            cmd->Num,
            cmd->Max,
            senderContext);

        if (wcsncmp(cmd->Data, kModPrefix, kModPrefixChars) != 0)
            return false;

        bool consumed = NetworkChannel::DispatchServerMessage(senderContext, cmd->Data, cmd->Num);
        if (!consumed)
        {
            ModLoaderLogger::LogWarn(
                L"[ServerChatCommit] DispatchServerMessage returned false for context=%p",
                senderContext);
        }

        if (allowNeuter)
        {
            // Prevent the original handler from logging "Command not recognized"
            // when we already identified the payload as mod-loader traffic.
            cmd->Data[0] = L'\0';
            cmd->Num = 1;
        }

        return consumed;
    }

    static void ProcessEventObserver(void* obj, void* fn, void* params)
    {
        if (fn != g_serverExecCmdFunc || !params)
            return;

        auto* cmd = reinterpret_cast<FStringView*>(params);
        if (TryDispatchEnvelope(obj, cmd, true))
        {
            ModLoaderLogger::LogTrace(
                L"[ServerChatCommit] ProcessEvent observer consumed mod envelope for context=%p",
                obj);
        }
    }

    static void __fastcall ExecHook(void* context, void* stack, void* result)
    {
        ModLoaderLogger::LogTrace(
            L"[ServerChatCommit] ExecHook enter context=%p stack=%p result=%p orig=%p",
            context,
            stack,
            result,
            reinterpret_cast<void*>(g_origExec));

        void* locals = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + kFFrameLocalsOffset);
        ModLoaderLogger::LogTrace(L"[ServerChatCommit] ExecHook locals=%p", locals);

        if (locals)
        {
            auto* cmd = reinterpret_cast<FStringView*>(locals);

            if (TryDispatchEnvelope(context, cmd, false))
            {
                return; // suppress original -- don't execute as a console command
            }
        }
        else
        {
            auto* node = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + 0x08);
            auto* object = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + 0x10);
            auto* code = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + 0x18);
            ModLoaderLogger::LogTrace(
                L"[ServerChatCommit] ExecHook has null locals; node=%p object=%p code=%p -- forwarding to original ExecFunction",
                node,
                object,
                code);
        }

        if (g_origExec)
        {
            ModLoaderLogger::LogTrace(L"[ServerChatCommit] Calling original ExecFunction for context=%p", context);
            g_origExec(context, stack, result);
        }
        else
        {
            ModLoaderLogger::LogError(L"[ServerChatCommit] Original ExecFunction pointer is null during ExecHook");
        }
    }

    bool Install()
    {
        if (g_hook.installed)
        {
            ModLoaderLogger::LogInfo(L"[ServerChatCommit] Install skipped; hook already installed");
            return true;
        }

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

        Hooks::GameInstanceInit::RegisterProcessEventCallback(&ProcessEventObserver);
        ModLoaderLogger::LogInfo(L"[ServerChatCommit] ProcessEvent observer registered");

        return ok;
    }

    void Remove()
    {
        Hooks::GameInstanceInit::UnregisterProcessEventCallback(&ProcessEventObserver);
        ModLoaderLogger::LogInfo(
            L"[ServerChatCommit] Removing ExecFunction hook (installed=%s)",
            g_hook.installed ? L"true" : L"false");
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
