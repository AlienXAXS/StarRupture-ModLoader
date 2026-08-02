#include "server_console.h"
#include "console_commands.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
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
    // -----------------------------------------------------------------------
    static HANDLE            s_out          = INVALID_HANDLE_VALUE;
    static HANDLE            s_in           = INVALID_HANDLE_VALUE;
    static HANDLE            s_thread       = NULL;
    static HANDLE            s_commandDone  = NULL;   // set by the dispatch completion callback
    static std::atomic<bool> s_running{ false };
    static std::atomic<bool> s_stopping{ false };
    static bool              s_ownsConsole  = false;  // we called AllocConsole (vs. attached to the engine's)
    static WORD              s_defaultAttrs = 0x07;

    // Guards writes to the console. The prompt is written by the reader thread
    // while command output can arrive from the game thread, so they must not
    // interleave mid-line.
    static std::mutex s_writeMutex;

    // How long to wait for a game-thread command before printing the prompt
    // again. Generous: a reload runs PluginShutdown, FreeLibrary, LoadLibrary
    // and PluginInit inside one tick, and the tick may be a while coming if the
    // server is still booting.
    static constexpr DWORD kCommandTimeoutMs = 30'000;

    // -----------------------------------------------------------------------
    // Low-level console output
    // -----------------------------------------------------------------------
    static void WriteRaw(const char* text, WORD attrs)
    {
        if (!text)
            return;

        // The handle is both tested and used under the lock: Shutdown() closes
        // it, and a handle value can be reused the moment it is closed. Writing
        // console text into whatever file inherited the number would be a far
        // worse outcome than dropping the line.
        std::lock_guard<std::mutex> lk(s_writeMutex);
        if (s_out == INVALID_HANDLE_VALUE)
            return;

        SetConsoleTextAttribute(s_out, attrs);
        DWORD written = 0;
        WriteConsoleA(s_out, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
        SetConsoleTextAttribute(s_out, s_defaultAttrs);
    }

    static void WriteLine(const char* text, WORD attrs)
    {
        std::string buf(text ? text : "");
        buf += "\r\n";
        WriteRaw(buf.c_str(), attrs);
    }

    static WORD AttrsFor(ModConsole::LineKind kind)
    {
        switch (kind)
        {
        case ModConsole::LineKind::Error:
            return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case ModConsole::LineKind::Notice:
            return FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        default:
            return s_defaultAttrs;
        }
    }

    // -----------------------------------------------------------------------
    // Sink -- prints straight to the console as lines arrive
    // -----------------------------------------------------------------------
    class ConsoleSink : public ModConsole::Sink
    {
    public:
        void Write(ModConsole::LineKind kind, const char* text) override
        {
            WriteLine(text, AttrsFor(kind));
        }

        void Clear() override
        {
            std::lock_guard<std::mutex> lk(s_writeMutex);
            if (s_out == INVALID_HANDLE_VALUE)
                return;

            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (!GetConsoleScreenBufferInfo(s_out, &info))
                return;

            const DWORD  cells  = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
            const COORD  origin = { 0, 0 };
            DWORD written = 0;

            FillConsoleOutputCharacterA(s_out, ' ', cells, origin, &written);
            FillConsoleOutputAttribute(s_out, s_defaultAttrs, cells, origin, &written);
            SetConsoleCursorPosition(s_out, origin);
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
    //
    // A console created here makes Ctrl+C terminate the game, which is a nasty
    // surprise for a key combination people press out of habit. Swallow it, and
    // let the genuinely terminal events (window closed, logoff, shutdown) fall
    // through to the default handler.
    // -----------------------------------------------------------------------
    static BOOL WINAPI CtrlHandler(DWORD ctrlType)
    {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
        {
            WriteLine("(Ctrl+C ignored -- close the game normally, or type 'exit' to close this console)",
                      FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            return TRUE;
        }
        return FALSE;
    }

    // -----------------------------------------------------------------------
    // Reader thread
    // -----------------------------------------------------------------------
    static void PrintBanner()
    {
        WriteLine("", s_defaultAttrs);
        WriteLine("  StarRupture Mod Loader console (" MODLOADER_BUILD_TAG ")",
                  FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        WriteLine("  Type 'help' for commands, 'exit' to close this window.",
                  FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        WriteLine("  Closing this window closes the game -- use 'exit' instead.",
                  FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        WriteLine("", s_defaultAttrs);
    }

    // Reads one line. Returns false when the console has gone away, which is
    // how the thread exits on Shutdown().
    static bool ReadLine(std::string& outLine)
    {
        wchar_t buffer[1024]{};
        DWORD   read = 0;

        if (!ReadConsoleW(s_in, buffer, static_cast<DWORD>(_countof(buffer) - 1), &read, nullptr))
            return false;
        if (read == 0)
            return false;

        buffer[read] = L'\0';

        // ReadConsole hands back the CRLF the user typed.
        while (read > 0 && (buffer[read - 1] == L'\r' || buffer[read - 1] == L'\n'))
            buffer[--read] = L'\0';

        char narrow[1024]{};
        WideCharToMultiByte(CP_ACP, 0, buffer, -1, narrow, sizeof(narrow), "?", nullptr);
        narrow[sizeof(narrow) - 1] = '\0';

        outLine = narrow;
        return true;
    }

    static std::string Trim(const std::string& s)
    {
        const size_t first = s.find_first_not_of(" \t");
        if (first == std::string::npos) return {};
        const size_t last = s.find_last_not_of(" \t");
        return s.substr(first, last - first + 1);
    }

    static DWORD WINAPI ReaderThreadProc(LPVOID)
    {
        auto sink = std::make_shared<ConsoleSink>();

        PrintBanner();

        while (!s_stopping.load())
        {
            WriteRaw("> ", FOREGROUND_GREEN | FOREGROUND_INTENSITY);

            std::string raw;
            if (!ReadLine(raw))
                break;

            const std::string line = Trim(raw);
            if (line.empty())
                continue;

            if (_stricmp(line.c_str(), "exit") == 0 || _stricmp(line.c_str(), "close") == 0)
            {
                WriteLine("Closing console. The game keeps running.", s_defaultAttrs);
                Shutdown();
                break;
            }

            ResetEvent(s_commandDone);

            if (!ModConsole::Dispatch(line, sink, []() { SetEvent(s_commandDone); }))
            {
                WriteLine("Unknown command. Type 'help' for the list.",
                          FOREGROUND_RED | FOREGROUND_INTENSITY);
                continue;
            }

            // Dispatch returns as soon as a game-thread command is queued, so
            // wait for it to actually run before prompting again -- otherwise
            // the prompt lands in the middle of the command's own output.
            if (WaitForSingleObject(s_commandDone, kCommandTimeoutMs) == WAIT_TIMEOUT)
            {
                WriteLine("Command is still queued on the game thread -- is the server ticking?",
                          FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            }
        }

        s_running.store(false);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------
    bool IsRunning() { return s_running.load(); }

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

        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(s_out, &info))
            s_defaultAttrs = info.wAttributes;

        // Line input mode gives us the console's own editing and F7 history for
        // free, which is worth more than anything a raw-mode reader would add.
        SetConsoleMode(s_in, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);

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

        // The reader thread is blocked inside ReadConsoleW and there is no way
        // to cancel that; closing the input handle and detaching the console
        // makes the call fail, which is how it gets out. It is never joined --
        // waiting on a thread parked in a console read is a hang, and by the
        // time this runs the process is on its way down anyway.
        if (s_in != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_in);
            s_in = INVALID_HANDLE_VALUE;
        }
        {
            // Under the write lock so a game-thread command printing its result
            // right now finishes first, and sees INVALID_HANDLE_VALUE after.
            std::lock_guard<std::mutex> lk(s_writeMutex);
            if (s_out != INVALID_HANDLE_VALUE)
            {
                CloseHandle(s_out);
                s_out = INVALID_HANDLE_VALUE;
            }
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

        // Left open deliberately: a command still queued on the game thread will
        // call SetEvent on it after this returns. Closing it here would make
        // that a use-after-free of a kernel handle.
        // s_commandDone is released only at process exit.
    }
}
