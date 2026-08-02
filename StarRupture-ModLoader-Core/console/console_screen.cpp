#include "console_screen.h"
#include "engine_output_hook.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace ConsoleScreen
{
    // -----------------------------------------------------------------------
    // Colour table
    //
    // 16-colour console attributes rather than VT escape sequences: the engine
    // sets console modes and text attributes for its own logging, and a
    // rendering path that depends on VT staying enabled is a rendering path
    // that breaks the first time something turns it off.
    // -----------------------------------------------------------------------
    static constexpr WORD kGrey      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    static constexpr WORD kWhite     = kGrey | FOREGROUND_INTENSITY;
    static constexpr WORD kRed       = FOREGROUND_RED | FOREGROUND_INTENSITY;
    static constexpr WORD kYellow    = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    static constexpr WORD kGreen     = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    static constexpr WORD kCyan      = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    static constexpr WORD kDim       = FOREGROUND_INTENSITY;   // dark grey
    static constexpr WORD kPromptCol = kGreen;

    // Whole-line severity, first match wins. Matched case-sensitively against
    // the engine's own conventions ("Warning:", "Error:") so an ordinary
    // sentence containing the word "warning" does not paint the line yellow.
    struct LineRule { const char* needle; WORD attrs; };

    static const LineRule kLineRules[] = {
        { "Fatal error",  kRed    },
        { "Fatal:",       kRed    },
        { "Error:",       kRed    },
        { "Warning:",     kYellow },
        { "Display:",     kWhite  },
        { "[ModLoader]",  kCyan   },
        { "VeryVerbose:", kDim    },
        { "Verbose:",     kDim    },
    };

    // Words highlighted inside a line, on top of whatever the line colour is.
    // Case-insensitive: these are the words you scan a wall of log for.
    struct WordRule { const char* word; WORD attrs; };

    static const WordRule kWordRules[] = {
        { "exception",  kRed    },
        { "crash",      kRed    },
        { "failed",     kRed    },
        { "failure",    kRed    },
        { "fatal",      kRed    },
        { "error",      kRed    },
        { "warning",    kYellow },
        { "timeout",    kYellow },
        { "deprecated", kYellow },
        { "success",    kGreen  },
        { "loaded",     kGreen  },
        { "reloaded",   kGreen  },
        { "unloaded",   kYellow },
        { "initialized",kGreen  },
        { "connected",  kGreen  },
        { "disconnect", kYellow },
    };

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    // One lock for the whole module: prompt state and output arrive on
    // different threads (reader thread, engine log threads, game thread) and
    // every one of them moves the cursor.
    static std::mutex   s_mutex;
    static HANDLE       s_out           = INVALID_HANDLE_VALUE;
    static WORD         s_defaultAttrs  = kGrey;
    static bool         s_promptVisible = false;
    static SHORT        s_promptRow     = 0;
    static int          s_width         = 80;

    static std::string  s_promptText    = "> ";
    static std::string  s_promptInput;
    static size_t       s_promptCaret   = 0;

    // Engine text arrives in whatever chunks the engine felt like writing;
    // everything up to the last newline is printed and the remainder waits here.
    static std::wstring s_pendingEngine;

    // -----------------------------------------------------------------------
    // Raw output -- always through the hook's trampoline, never WriteConsoleW
    // directly, or our own drawing would re-enter the detour.
    // -----------------------------------------------------------------------
    static void RawWrite(const wchar_t* text, size_t len)
    {
        if (s_out == INVALID_HANDLE_VALUE || !text || len == 0)
            return;

        DWORD written = 0;
        EngineOutputHook::RawWriteW(s_out, text, static_cast<DWORD>(len), &written);
    }

    static void RawWriteAscii(const char* text, size_t len, WORD attrs)
    {
        if (!text || len == 0)
            return;

        std::wstring wide;
        wide.reserve(len);
        for (size_t i = 0; i < len; ++i)
            wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(text[i])));

        SetConsoleTextAttribute(s_out, attrs);
        RawWrite(wide.c_str(), wide.size());
    }

    static void RawNewline()
    {
        RawWrite(L"\r\n", 2);
    }

    static void RefreshMetrics()
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(s_out, &info))
            s_width = info.dwSize.X > 0 ? info.dwSize.X : 80;
    }

    // -----------------------------------------------------------------------
    // Prompt
    // -----------------------------------------------------------------------
    static void ErasePromptLocked()
    {
        if (!s_promptVisible || s_out == INVALID_HANDLE_VALUE)
            return;

        const COORD start = { 0, s_promptRow };
        DWORD written = 0;
        FillConsoleOutputCharacterW(s_out, L' ', static_cast<DWORD>(s_width), start, &written);
        FillConsoleOutputAttribute(s_out, s_defaultAttrs, static_cast<DWORD>(s_width), start, &written);
        SetConsoleCursorPosition(s_out, start);
        s_promptVisible = false;
    }

    static void DrawPromptLocked()
    {
        if (s_out == INVALID_HANDLE_VALUE)
            return;

        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(s_out, &info))
            return;

        s_width     = info.dwSize.X > 0 ? info.dwSize.X : 80;
        s_promptRow = info.dwCursorPosition.Y;

        const int promptLen = static_cast<int>(s_promptText.size());

        // The input scrolls horizontally inside what is left of the row instead
        // of wrapping onto a second one. A prompt that is always exactly one
        // row high is a prompt whose erase can never miss part of itself --
        // and a wrapped line that scrolls the buffer moves the row out from
        // under s_promptRow.
        const int space = s_width - promptLen - 1;
        if (space <= 0)
            return;

        size_t first = 0;
        if (static_cast<int>(s_promptCaret) >= space)
            first = s_promptCaret - static_cast<size_t>(space) + 1;

        const std::string visible = s_promptInput.substr(first, static_cast<size_t>(space));

        RawWriteAscii(s_promptText.c_str(), s_promptText.size(), kPromptCol);
        RawWriteAscii(visible.c_str(), visible.size(), kWhite);
        SetConsoleTextAttribute(s_out, s_defaultAttrs);

        const COORD caretPos = {
            static_cast<SHORT>(promptLen + static_cast<int>(s_promptCaret - first)),
            s_promptRow
        };
        SetConsoleCursorPosition(s_out, caretPos);

        s_promptVisible = true;
    }

    // -----------------------------------------------------------------------
    // Line rendering
    // -----------------------------------------------------------------------
    static WORD LineAttrsFor(const std::string& line, WORD fallback)
    {
        for (const LineRule& rule : kLineRules)
        {
            if (line.find(rule.needle) != std::string::npos)
                return rule.attrs;
        }
        return fallback;
    }

    static bool IsWordChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    // Finds the next highlight-word match at or after `from`. Matches on whole
    // words only, so "error" does not light up inside "errorless" or a file path.
    static bool NextHighlight(const std::string& line, size_t from,
                              size_t& outPos, size_t& outLen, WORD& outAttrs)
    {
        bool found = false;
        outPos = std::string::npos;

        for (const WordRule& rule : kWordRules)
        {
            const size_t wordLen = strlen(rule.word);

            for (size_t i = from; i + wordLen <= line.size(); ++i)
            {
                if (_strnicmp(line.c_str() + i, rule.word, wordLen) != 0)
                    continue;
                if (i > 0 && IsWordChar(line[i - 1]))
                    continue;
                if (i + wordLen < line.size() && IsWordChar(line[i + wordLen]))
                    continue;

                if (!found || i < outPos)
                {
                    found    = true;
                    outPos   = i;
                    outLen   = wordLen;
                    outAttrs = rule.attrs;
                }
                break;   // earliest match for this word is enough
            }
        }
        return found;
    }

    // Writes one line, base colour plus inline highlights. Assumes the prompt
    // is already erased and the caller holds the lock.
    static void WriteLineLocked(const std::string& line, WORD baseAttrs)
    {
        size_t cursor = 0;
        size_t pos = 0, len = 0;
        WORD   attrs = baseAttrs;

        while (NextHighlight(line, cursor, pos, len, attrs))
        {
            if (pos > cursor)
                RawWriteAscii(line.c_str() + cursor, pos - cursor, baseAttrs);
            RawWriteAscii(line.c_str() + pos, len, attrs);
            cursor = pos + len;
        }

        if (cursor < line.size())
            RawWriteAscii(line.c_str() + cursor, line.size() - cursor, baseAttrs);

        SetConsoleTextAttribute(s_out, s_defaultAttrs);
        RawNewline();
    }

    static WORD AttrsForKind(ModConsole::LineKind kind)
    {
        switch (kind)
        {
        case ModConsole::LineKind::Error:  return kRed;
        case ModConsole::LineKind::Notice: return kCyan;
        default:                           return s_defaultAttrs;
        }
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------
    bool Init(HANDLE out)
    {
        std::lock_guard<std::mutex> lk(s_mutex);

        if (out == INVALID_HANDLE_VALUE)
            return false;

        s_out = out;

        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(s_out, &info))
        {
            s_defaultAttrs = info.wAttributes;
            s_width        = info.dwSize.X > 0 ? info.dwSize.X : 80;
            s_promptRow    = info.dwCursorPosition.Y;
        }

        s_promptVisible = false;
        s_pendingEngine.clear();
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lk(s_mutex);

        if (s_out != INVALID_HANDLE_VALUE)
        {
            ErasePromptLocked();
            SetConsoleTextAttribute(s_out, s_defaultAttrs);
        }

        s_out           = INVALID_HANDLE_VALUE;
        s_promptVisible = false;
        s_pendingEngine.clear();
    }

    bool IsActive()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        return s_out != INVALID_HANDLE_VALUE;
    }

    int Width()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        return s_width;
    }

    void Print(ModConsole::LineKind kind, const char* text)
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_out == INVALID_HANDLE_VALUE)
            return;

        const bool hadPrompt = s_promptVisible;
        ErasePromptLocked();

        const std::string line(text ? text : "");
        WriteLineLocked(line, AttrsForKind(kind));

        if (hadPrompt)
            DrawPromptLocked();
    }

    void PrintEngineText(const wchar_t* text, size_t len)
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_out == INVALID_HANDLE_VALUE || !text || len == 0)
            return;

        s_pendingEngine.append(text, len);

        // Only complete lines are printed, so a line the engine wrote in two
        // calls is still colourised as one. A pathological writer that never
        // sends a newline would otherwise buffer forever, hence the cap.
        static constexpr size_t kMaxPending = 8192;
        if (s_pendingEngine.size() > kMaxPending && s_pendingEngine.find(L'\n') == std::wstring::npos)
            s_pendingEngine.push_back(L'\n');

        if (s_pendingEngine.find(L'\n') == std::wstring::npos)
            return;

        const bool hadPrompt = s_promptVisible;
        ErasePromptLocked();

        size_t start = 0;
        size_t nl    = 0;
        while ((nl = s_pendingEngine.find(L'\n', start)) != std::wstring::npos)
        {
            std::wstring wline = s_pendingEngine.substr(start, nl - start);
            if (!wline.empty() && wline.back() == L'\r')
                wline.pop_back();

            std::string line;
            line.reserve(wline.size());
            for (wchar_t wc : wline)
                line.push_back(wc < 128 ? static_cast<char>(wc) : '?');

            WriteLineLocked(line, LineAttrsFor(line, kGrey));
            start = nl + 1;
        }
        s_pendingEngine.erase(0, start);

        if (hadPrompt)
            DrawPromptLocked();
    }

    void SetPrompt(const char* prompt, const std::string& input, size_t caret)
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_out == INVALID_HANDLE_VALUE)
            return;

        if (prompt)
            s_promptText = prompt;
        s_promptInput = input;
        s_promptCaret = caret > input.size() ? input.size() : caret;

        // Erase in place rather than redrawing over the old text: the new input
        // can be shorter, and the tail of the old line would otherwise stay on
        // screen.
        if (s_promptVisible)
        {
            const COORD start = { 0, s_promptRow };
            DWORD written = 0;
            FillConsoleOutputCharacterW(s_out, L' ', static_cast<DWORD>(s_width), start, &written);
            FillConsoleOutputAttribute(s_out, s_defaultAttrs, static_cast<DWORD>(s_width), start, &written);
            SetConsoleCursorPosition(s_out, start);
            s_promptVisible = false;
        }

        DrawPromptLocked();
    }

    void RedrawPrompt()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_out == INVALID_HANDLE_VALUE)
            return;

        RefreshMetrics();
        ErasePromptLocked();
        DrawPromptLocked();
    }

    void ClearScreen()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_out == INVALID_HANDLE_VALUE)
            return;

        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(s_out, &info))
            return;

        const DWORD cells  = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
        const COORD origin = { 0, 0 };
        DWORD written = 0;

        FillConsoleOutputCharacterW(s_out, L' ', cells, origin, &written);
        FillConsoleOutputAttribute(s_out, s_defaultAttrs, cells, origin, &written);
        SetConsoleCursorPosition(s_out, origin);

        s_promptVisible = false;
        DrawPromptLocked();
    }
}
