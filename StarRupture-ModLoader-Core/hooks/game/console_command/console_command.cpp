#include "pch.h"
#include "console_command.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "hooks/memory/engine_allocator.h"
#include "Engine_classes.hpp"   // UWorld, UGameplayStatics
#include "../scan_patterns.h"
#include <cstdint>

namespace Hooks::ConsoleCommand
{
    // Mirrors FString's layout (TArray<wchar_t>): data pointer, element count
    // including the null terminator, and capacity.
    struct FStringPod
    {
        wchar_t* Data;
        int32_t  Num;
        int32_t  Max;
    };
    static_assert(sizeof(FStringPod) == 0x10, "FStringPod size mismatch");

    using ConsoleCommand_t = FStringPod*(__fastcall*)(void* thisPtr,
                                                      FStringPod* outResult,
                                                      const FStringPod* command,
                                                      bool bWriteToLog);

    static ConsoleCommand_t g_consoleCommand = nullptr;
    static bool             g_scanAttempted  = false;

    bool IsAvailable() { return g_consoleCommand != nullptr; }

    bool Resolve()
    {
        if (g_consoleCommand)  return true;
        if (g_scanAttempted)   return false;
        g_scanAttempted = true;

        const uintptr_t addr = Scanner::FindPatternInMainModule(
            "APlayerController::ConsoleCommand",
            ScanPatterns::APlayerController_ConsoleCommand);

        if (!addr)
        {
            ModLoaderLogger::LogWarn(
                L"[ConsoleCommand] [FAIL] APlayerController::ConsoleCommand pattern not found "
                L"-- the developer console will be unable to run commands");
            return false;
        }

        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        g_consoleCommand = reinterpret_cast<ConsoleCommand_t>(addr);
        ModLoaderLogger::LogInfo(
            L"[ConsoleCommand] [OK] APlayerController::ConsoleCommand at 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr - base));
        return true;
    }

    // Native call kept in its own SEH-guarded helper with no C++ objects in
    // scope (C2712).
    //
    // bWriteToLog MUST be false. UPlayer::ConsoleCommand ends with
    //   if (bWriteToLog) { result = TEXT(""); } else { result = <captured>; }
    // -- passing true routes the output to the log and hands back an empty
    // string, which is exactly the "it appeared in the UE log but not in our
    // window" symptom.
    static bool CallConsoleCommandSEH(void* pc, FStringPod* out, const FStringPod* cmd)
    {
        __try
        {
            g_consoleCommand(pc, out, cmd, /*bWriteToLog*/ false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool Execute(const wchar_t* command, std::wstring& outResult)
    {
        outResult.clear();

        if (!command || !command[0])
            return false;

        if (!Resolve())
            return false;

        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
        {
            ModLoaderLogger::LogWarn(L"[ConsoleCommand] No UWorld -- cannot execute");
            return false;
        }

        void* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
        if (!pc)
        {
            ModLoaderLogger::LogWarn(L"[ConsoleCommand] No local PlayerController -- cannot execute");
            return false;
        }

        // The engine only reads the input FString, so backing it with our own
        // buffer is safe (same pattern as network_channel's RPC sends).
        std::wstring cmd(command);
        const int32_t len = static_cast<int32_t>(cmd.size()) + 1;  // include null

        FStringPod in{};
        in.Data = cmd.data();
        in.Num  = len;
        in.Max  = len;

        FStringPod out{};

        ModLoaderLogger::LogDebug(L"[ConsoleCommand] Executing: %s", command);

        if (!CallConsoleCommandSEH(pc, &out, &in))
        {
            ModLoaderLogger::LogError(L"[ConsoleCommand] Exception while executing: %s", command);
            return false;
        }

        // The returned buffer is engine-allocated; copy it out then hand it back.
        if (out.Data && out.Num > 1)
            outResult.assign(out.Data, static_cast<size_t>(out.Num) - 1);

        if (out.Data)
        {
            if (EngineAllocator::IsAvailable())
                EngineAllocator::Free(out.Data);
            else
                ModLoaderLogger::LogWarn(
                    L"[ConsoleCommand] Engine allocator unavailable -- leaking the returned FString");
        }

        return true;
    }
}

#endif // MODLOADER_CLIENT_BUILD
