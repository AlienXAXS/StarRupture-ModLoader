#include "server_console.h"
#include "console_commands.h"
#include "console_input.h"
#include "console_screen.h"
#include "engine_output_hook.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include "logging/logger.h"
#include "utils/game_thread_dispatch.h"

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

namespace ServerConsole
{
    // -----------------------------------------------------------------------
    // State
    //
    // Drawing lives in ConsoleScreen and editing in ConsoleInput; what is left
    // here is the console's lifetime and the read-dispatch loop.
    // -----------------------------------------------------------------------
    static HANDLE            s_out         = INVALID_HANDLE_VALUE;
    static HANDLE            s_in          = INVALID_HANDLE_VALUE;
    static HANDLE            s_thread      = NULL;
    static HANDLE            s_commandDone = NULL;   // set by the dispatch completion callback
    static std::atomic<bool> s_running{ false };
    static std::atomic<bool> s_stopping{ false };
    static bool              s_ownsConsole = false;  // we called AllocConsole (vs. attached to the engine's)

    // How long to wait for a game-thread command before prompting again.
    // Generous: a reload runs PluginShutdown, FreeLibrary, LoadLibrary and
    // PluginInit inside one tick, and the tick may be a while coming if the
    // server is still booting.
    static constexpr DWORD kCommandTimeoutMs = 30'000;

    // -----------------------------------------------------------------------
    // Sink -- command output goes to the screen, above the prompt
    // -----------------------------------------------------------------------
    class ConsoleSink : public ModConsole::Sink
    {
    public:
        void Write(ModConsole::LineKind kind, const char* text) override
        {
            ConsoleScreen::Print(kind, text ? text : "");
        }

        void Clear() override
        {
            ConsoleScreen::ClearScreen();
        }
    };

    // -----------------------------------------------------------------------
    // Command line
    // -----------------------------------------------------------------------
    // Whole-token match so -consoleblah or a path containing "-console" does not
    // count. UE arguments are separated by spaces; quoted paths are irrelevant
    // here because we only ever compare against bare switches.
    static bool HasSwitch(const wchar_t* cmdLine, const wchar_t* name)
    {
        const wchar_t* p = cmdLine;
        const size_t   nameLen = wcslen(name);

        while (*p)
        {
            while (*p == L' ' || *p == L'\t') ++p;
            const wchar_t* start = p;
            while (*p && *p != L' ' && *p != L'\t') ++p;

            const size_t tokenLen = static_cast<size_t>(p - start);
            if (tokenLen == nameLen && _wcsnicmp(start, name, nameLen) == 0)
                return true;
        }
        return false;
    }

    bool IsRequested()
    {
        const wchar_t* cmdLine = GetCommandLineW();
        return HasSwitch(cmdLine, L"-console") ||
               HasSwitch(cmdLine, L"-mlconsole") ||
               HasSwitch(cmdLine, L"-modloaderconsole");
    }

    // -----------------------------------------------------------------------
    // Ctrl handling
    // -----------------------------------------------------------------------
    // Set by RequestGracefulProcessExit just before it raises the event, so the
    // handler below knows this particular Ctrl+C is ours and must be passed on
    // to the engine rather than swallowed.
    static std::atomic<bool> s_selfStopRequested{ false };

    // Keyboard Ctrl+C never reaches here any more (ConsoleInput turns off
    // PROCESSED_INPUT and treats it as "clear the line"), so this now sees only
    // events raised by another process -- a batch file being interrupted, say --
    // and the one `stop` raises itself.
    static BOOL WINAPI CtrlHandler(DWORD ctrlType)
    {
        if (ctrlType == CTRL_C_EVENT && s_selfStopRequested.exchange(false))
        {
            // Ours: fall through to the engine's graceful termination handler,
            // which is the entire point of raising the event.
            return FALSE;
        }

        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
        {
            ConsoleScreen::Print(ModConsole::LineKind::Error,
                                 "(Ctrl+C ignored -- type 'stop' to shut the server down cleanly)");
            return TRUE;
        }
        return FALSE;
    }

    // -----------------------------------------------------------------------
    // Reader thread
    // -----------------------------------------------------------------------
    static void PrintBanner()
    {
        ConsoleScreen::Print(ModConsole::LineKind::Output, "");
        ConsoleScreen::Print(ModConsole::LineKind::Notice,
                             "  StarRupture Mod Loader console (" MODLOADER_BUILD_TAG ")");
        ConsoleScreen::Print(ModConsole::LineKind::Notice,
                             "  'help' for commands, Tab completes, Up/Down for history.");
        ConsoleScreen::Print(ModConsole::LineKind::Notice,
                             "  'stop' shuts the server down cleanly. Closing this window kills it.");
        ConsoleScreen::Print(ModConsole::LineKind::Output, "");
    }

    static DWORD WINAPI ReaderThreadProc(LPVOID)
    {
        auto sink = std::make_shared<ConsoleSink>();

        PrintBanner();

        while (!s_stopping.load())
        {
            std::string line;
            if (ConsoleInput::ReadLine(line) != ConsoleInput::Result::Line)
                break;

            // Trim -- the editor keeps exactly what was typed.
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos)
                continue;
            line = line.substr(first, line.find_last_not_of(" \t") - first + 1);
            if (line.empty())
                continue;

            // 'exit' and 'quit' mean opposite things to different people -- one
            // camp expects the server to stop, the other expects the window to
            // go away. Both are destructive if guessed wrong (a dead server, or
            // a live server with no console left to control it), so neither
            // word picks a side.
            if (_stricmp(line.c_str(), "exit") == 0 || _stricmp(line.c_str(), "quit") == 0)
            {
                ConsoleScreen::Print(ModConsole::LineKind::Error,
                    "Ambiguous: 'stop' shuts the server down, 'closeconsole' just closes this window.");
                continue;
            }

            if (_stricmp(line.c_str(), "closeconsole") == 0 || _stricmp(line.c_str(), "close") == 0)
            {
                ConsoleScreen::Print(ModConsole::LineKind::Output,
                    "Closing console. The server keeps running -- it cannot be reopened.");
                Shutdown();
                break;
            }

            ResetEvent(s_commandDone);

            if (!ModConsole::Dispatch(line, sink, []() { SetEvent(s_commandDone); }))
            {
                ConsoleScreen::Print(ModConsole::LineKind::Error,
                                     "Unknown command. Type 'help' for the list.");
                continue;
            }

            // Dispatch returns as soon as a game-thread command is queued, so
            // wait for it to actually run before reading again -- otherwise the
            // next thing typed races the output of the last thing.
            if (WaitForSingleObject(s_commandDone, kCommandTimeoutMs) == WAIT_TIMEOUT)
            {
                ConsoleScreen::Print(ModConsole::LineKind::Error,
                    "Command is still queued on the game thread -- is the server ticking?");
            }
        }

        s_running.store(false);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------
    bool IsRunning() { return s_running.load(); }

    bool RequestGracefulProcessExit()
    {
        if (!GetConsoleWindow())
        {
            ModLoaderLogger::LogError(L"[Console] Cannot request shutdown: the process has no console");
            return false;
        }

        ModLoaderLogger::LogInfo(L"[Console] Graceful shutdown requested -- raising Ctrl+C for the engine");

        // Set before raising it: the handler runs on a thread the system
        // creates for the event, and it can be in there before this returns.
        s_selfStopRequested.store(true);

        if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        {
            s_selfStopRequested.store(false);
            ModLoaderLogger::LogError(L"[Console] GenerateConsoleCtrlEvent failed (%lu)", GetLastError());
            return false;
        }

        return true;
    }

    void Start()
    {
        if (s_running.load())
            return;

        if (!IsRequested())
            return;

        // AllocConsole fails when the process already has one -- the engine's
        // own, if the game was started with -log. Attach to that instead of
        // giving up; sharing a window beats having no console at all.
        if (AllocConsole())
        {
            s_ownsConsole = true;
        }
        else
        {
            const DWORD err = GetLastError();
            if (!GetConsoleWindow())
            {
                ModLoaderLogger::LogError(
                    L"[Console] -console requested but AllocConsole failed (%lu) and there is no console to attach to",
                    err);
                return;
            }
            ModLoaderLogger::LogInfo(L"[Console] Attaching to the existing process console");
        }

        // CONOUT$/CONIN$ rather than the std handles: the game may have
        // redirected stdio, and we want the console either way.
        s_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        s_in  = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);

        if (s_out == INVALID_HANDLE_VALUE || s_in == INVALID_HANDLE_VALUE)
        {
            ModLoaderLogger::LogError(L"[Console] Failed to open CONOUT$/CONIN$ (%lu) -- console disabled",
                                      GetLastError());
            Shutdown();
            return;
        }

        if (!ConsoleScreen::Init(s_out))
        {
            ModLoaderLogger::LogError(L"[Console] Screen init failed -- console disabled");
            Shutdown();
            return;
        }

        if (!ConsoleInput::Begin(s_in))
        {
            ModLoaderLogger::LogError(L"[Console] Could not switch the console to raw input (%lu) -- console disabled",
                                      GetLastError());
            Shutdown();
            return;
        }

        // Installed before the reader thread starts so the engine's very first
        // log line after this point already goes through the renderer, rather
        // than landing on top of a prompt that is about to be drawn.
        EngineOutputHook::Install();

        if (s_ownsConsole)
            SetConsoleTitleW(L"StarRupture Mod Loader Console");
        SetConsoleCtrlHandler(&CtrlHandler, TRUE);

        s_commandDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!s_commandDone)
        {
            ModLoaderLogger::LogError(L"[Console] Failed to create the command-completion event (%lu) -- console disabled",
                                      GetLastError());
            Shutdown();
            return;
        }

        ModConsole::RegisterBuiltins();

        s_stopping.store(false);
        s_running.store(true);

        s_thread = CreateThread(nullptr, 0, ReaderThreadProc, nullptr, 0, nullptr);
        if (!s_thread)
        {
            ModLoaderLogger::LogError(L"[Console] Failed to create the console reader thread (%lu)", GetLastError());
            s_running.store(false);
            Shutdown();
            return;
        }

        ModLoaderLogger::LogInfo(L"[Console] Mod loader console started (%s)",
                                 s_ownsConsole ? L"own window" : L"shared with the engine");
    }

    void Shutdown()
    {
        s_stopping.store(true);
        s_running.store(false);

        // Screen first: that is what makes the output hook a bare pass-through,
        // so engine logging keeps working normally through everything below.
        ConsoleScreen::Shutdown();
        EngineOutputHook::Remove();
        ConsoleInput::End();

        // The reader thread is blocked inside ReadConsoleInput and there is no
        // way to cancel that; closing the input handle makes the call fail,
        // which is how it gets out. It is never joined -- waiting on a thread
        // parked in a console read is a hang, and by the time this runs the
        // process is on its way down anyway.
        if (s_in != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_in);
            s_in = INVALID_HANDLE_VALUE;
        }
        if (s_out != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_out);
            s_out = INVALID_HANDLE_VALUE;
        }

        SetConsoleCtrlHandler(&CtrlHandler, FALSE);

        if (s_ownsConsole)
        {
            FreeConsole();
            s_ownsConsole = false;
        }

        if (s_thread)
        {
            CloseHandle(s_thread);
            s_thread = NULL;
        }

        // s_commandDone is left open deliberately: a command still queued on the
        // game thread will call SetEvent on it after this returns, and closing
        // it here would make that a use-after-free of a kernel handle.
    }
}
