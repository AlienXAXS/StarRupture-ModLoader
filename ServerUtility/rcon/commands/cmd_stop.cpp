#include "cmd_stop.h"
#include "command_handler.h"
#include "plugin_helpers.h"

#include <windows.h>

namespace Cmd_Stop
{
    // FWindowsPlatformMisc::RequestExit(bool Force, const wchar_t* CallSite)
    // Pattern: 48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 44 0F B6 05
    static constexpr const char* REQUEST_EXIT_PATTERN =
        "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 44 0F B6 05";

    using RequestExit_t = void(__fastcall*)(bool bForce, const wchar_t* CallSite);
    static RequestExit_t g_requestExit = nullptr;

    static std::string Handle(const std::string& /*args*/)
    {
        LOG_INFO("[RCON] Shutdown command received via RCON.");

        if (!g_requestExit)
        {
            LOG_ERROR("[RCON] RequestExit function not resolved - cannot shut down gracefully.");
            return "Error: graceful shutdown unavailable (RequestExit not found).\n";
        }

        // Spawn a brief-delay thread so the response packet is flushed
        // back to the client before the shutdown signal is raised.
        HANDLE hThread = CreateThread(
            nullptr, 0,
            [](LPVOID) -> DWORD
            {
                Sleep(300);

                LOG_INFO("[RCON] Calling FWindowsPlatformMisc::RequestExit(false) for graceful shutdown...");

                // Request a graceful (non-forced) exit.  This sets
                // GIsRequestingExit = true which the engine loop picks up on
                // the next tick, triggering the full save-and-shutdown path
                // (identical to pressing Ctrl+C in the server console).
                g_requestExit(false, L"RCON stop command");

                return 0;
            },
            nullptr, 0, nullptr);

        if (hThread)
            CloseHandle(hThread);

        return "Server is shutting down gracefully...\n";
    }

    void Register(CommandHandler& handler)
    {
        // Resolve RequestExit at registration time (engine is already up)
        auto scanner = GetScanner();
        if (scanner)
        {
            uintptr_t addr = scanner->FindPatternInMainModule(REQUEST_EXIT_PATTERN);
            if (addr)
            {
                g_requestExit = reinterpret_cast<RequestExit_t>(addr);
                LOG_INFO("[RCON] FWindowsPlatformMisc::RequestExit resolved at 0x%llX",
                         static_cast<unsigned long long>(addr));
            }
            else
            {
                LOG_ERROR("[RCON] Failed to find FWindowsPlatformMisc::RequestExit pattern - "
                          "stop command will not work");
            }
        }

        handler.Register(
            { "stop", "quit", "exit", "shutdown" },
            "Gracefully shut down the dedicated server (saves world first)",
            Handle);
    }
}
