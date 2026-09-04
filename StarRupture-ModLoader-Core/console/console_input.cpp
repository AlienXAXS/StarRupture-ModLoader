#include "console_input.h"
#include "console_screen.h"
#include "console_commands.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "plugins/plugin_manager.h"

namespace ConsoleInput
{
    static constexpr const char* kPrompt = "> ";

    static HANDLE s_in           = INVALID_HANDLE_VALUE;
    static DWORD  s_previousMode = 0;
    static bool   s_modeSaved    = false;

    static std::string              s_buffer;
    static size_t                   s_caret = 0;
    static std::vector<std::string> s_history;
    static int                      s_historyPos = -1;   // -1 == editing a fresh line
    static std::string              s_stashed;           // the fresh line, while browsing history

    // -----------------------------------------------------------------------
    // Mode
    // -----------------------------------------------------------------------
    bool Begin(HANDLE in)
    {
        s_in = in;
        if (s_in == INVALID_HANDLE_VALUE)
            return false;

        if (GetConsoleMode(s_in, &s_previousMode))
            s_modeSaved = true;

        // No LINE_INPUT or ECHO_INPUT: this module draws the line itself.
        // No PROCESSED_INPUT either, so Ctrl+C arrives as a keystroke we can
        // treat as "clear the line" instead of a signal that stops the server
        // -- `stop` is the only way to do that, and it still works because
        // GenerateConsoleCtrlEvent does not go through the input mode.
        // QUICK_EDIT stays on so log text can still be selected and copied.
        const DWORD mode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE;
        if (!SetConsoleMode(s_in, mode))
            return false;

        s_buffer.clear();
        s_caret      = 0;
        s_historyPos = -1;
        return true;
    }

    void End()
    {
        if (s_modeSaved && s_in != INVALID_HANDLE_VALUE)
            SetConsoleMode(s_in, s_previousMode);
        s_modeSaved = false;
        s_in        = INVALID_HANDLE_VALUE;
    }

    // -----------------------------------------------------------------------
    // Editing helpers
    // -----------------------------------------------------------------------
    static void Refresh()
    {
        ConsoleScreen::SetPrompt(kPrompt, s_buffer, s_caret);
    }

    static bool IsWordChar(char c)
    {
        return c != ' ' && c != '\t';
    }

    static size_t PrevWord(size_t from)
    {
        while (from > 0 && !IsWordChar(s_buffer[from - 1])) --from;
        while (from > 0 &&  IsWordChar(s_buffer[from - 1])) --from;
        return from;
    }

    static size_t NextWord(size_t from)
    {
        const size_t n = s_buffer.size();
        while (from < n &&  IsWordChar(s_buffer[from])) ++from;
        while (from < n && !IsWordChar(s_buffer[from])) ++from;
        return from;
    }

    static void LoadHistory(int pos)
    {
        if (pos < 0)
        {
            s_buffer = s_stashed;
        }
        else
        {
            s_buffer = s_history[static_cast<size_t>(pos)];
        }
        s_historyPos = pos;
        s_caret      = s_buffer.size();
        Refresh();
    }

    // -----------------------------------------------------------------------
    // Tab completion
    // -----------------------------------------------------------------------
    static std::vector<std::string> PluginNameCandidates()
    {
        std::vector<std::string> out;

        const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
        if (total > 0)
        {
            std::vector<PluginManager::PluginStatus> statuses(static_cast<size_t>(total));
            PluginManager::GetAllPluginStatuses(statuses.data(), total);
            for (const PluginManager::PluginStatus& s : statuses)
            {
                if (s.name[0])
                    out.push_back(s.name);
                else if (s.fileName[0])
                    out.push_back(s.fileName);
            }
        }
        out.push_back("all");
        return out;
    }

    // Candidates for the word being completed, given the command it belongs to.
    static std::vector<std::string> CandidatesFor(const std::vector<std::string>& tokens, bool completingFirst)
    {
        std::vector<std::string> out;

        if (completingFirst)
        {
            for (const ModConsole::CommandInfo& cmd : ModConsole::GetCommands())
                out.push_back(cmd.name);
            return out;
        }

        const std::string& verb = tokens[0];

        if (_stricmp(verb.c_str(), "reload") == 0 ||
            _stricmp(verb.c_str(), "unload") == 0 ||
            _stricmp(verb.c_str(), "load")   == 0)
            return PluginNameCandidates();

        if (_stricmp(verb.c_str(), "loglevel") == 0 || _stricmp(verb.c_str(), "log") == 0)
            return { "trace", "debug", "info", "warn", "error" };

        if (_stricmp(verb.c_str(), "stop") == 0)
            return { "force" };

        if (_stricmp(verb.c_str(), "help") == 0)
        {
            for (const ModConsole::CommandInfo& cmd : ModConsole::GetCommands())
                out.push_back(cmd.name);
        }
        return out;
    }

    // Longest prefix shared by every candidate, preserving the first one's case.
    static std::string CommonPrefix(const std::vector<std::string>& items)
    {
        if (items.empty()) return {};

        std::string prefix = items[0];
        for (size_t i = 1; i < items.size(); ++i)
        {
            size_t j = 0;
            while (j < prefix.size() && j < items[i].size() &&
                   tolower(static_cast<unsigned char>(prefix[j])) ==
                   tolower(static_cast<unsigned char>(items[i][j])))
                ++j;
            prefix.resize(j);
        }
        return prefix;
    }

    static void Complete()
    {
        // Only the word the caret sits at the end of; completing mid-line would
        // need to know where the word ends, and nobody tabs mid-word.
        size_t start = 0;
        if (s_caret > 0)
        {
            const size_t wordStart = s_buffer.find_last_of(" \t", s_caret - 1);
            start = (wordStart == std::string::npos) ? 0 : wordStart + 1;
        }
        if (start > s_caret)
            return;

        const std::string word = s_buffer.substr(start, s_caret - start);

        std::vector<std::string> tokens = ModConsole::Tokenize(s_buffer.substr(0, start));
        const bool completingFirst = tokens.empty();
        if (!completingFirst && tokens[0].empty())
            return;

        std::vector<std::string> candidates = CandidatesFor(tokens, completingFirst);

        std::vector<std::string> matches;
        for (const std::string& c : candidates)
        {
            if (word.empty() || _strnicmp(c.c_str(), word.c_str(), word.size()) == 0)
                matches.push_back(c);
        }

        if (matches.empty())
            return;

        std::string replacement = (matches.size() == 1) ? matches[0] : CommonPrefix(matches);
        if (replacement.size() < word.size())
            return;

        // A name with a space in it has to come back as one argument.
        const bool needsQuotes = matches.size() == 1 &&
                                 replacement.find(' ') != std::string::npos;
        if (needsQuotes)
            replacement = "\"" + replacement + "\"";

        s_buffer.replace(start, s_caret - start, replacement);
        s_caret = start + replacement.size();

        if (matches.size() == 1)
        {
            s_buffer.insert(s_caret, " ");
            ++s_caret;
        }
        else
        {
            // Show what the choices are, above the prompt.
            std::string line;
            for (const std::string& m : matches)
            {
                if (!line.empty()) line += "   ";
                line += m;
            }
            ConsoleScreen::Print(ModConsole::LineKind::Notice, line.c_str());
        }

        Refresh();
    }

    // -----------------------------------------------------------------------
    // Read loop
    // -----------------------------------------------------------------------
    static bool CtrlDown(DWORD state)
    {
        return (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    }

    Result ReadLine(std::string& outLine)
    {
        Refresh();

        for (;;)
        {
            INPUT_RECORD record{};
            DWORD read = 0;

            if (!ReadConsoleInputW(s_in, &record, 1, &read) || read == 0)
                return Result::Interrupted;

            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
            {
                ConsoleScreen::RedrawPrompt();
                continue;
            }

            if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
                continue;

            const KEY_EVENT_RECORD& key = record.Event.KeyEvent;

            for (WORD repeat = 0; repeat < (key.wRepeatCount ? key.wRepeatCount : 1); ++repeat)
            {
                const bool ctrl = CtrlDown(key.dwControlKeyState);

                switch (key.wVirtualKeyCode)
                {
                case VK_RETURN:
                {
                    outLine = s_buffer;

                    if (!outLine.empty())
                    {
                        if (s_history.empty() || s_history.back() != outLine)
                            s_history.push_back(outLine);
                    }

                    // Echo the command into the scrollback before clearing, so
                    // the output below it has something to belong to.
                    const std::string echo = std::string(kPrompt) + outLine;
                    s_buffer.clear();
                    s_caret      = 0;
                    s_historyPos = -1;
                    ConsoleScreen::SetPrompt(kPrompt, s_buffer, s_caret);
                    if (!outLine.empty())
                        ConsoleScreen::Print(ModConsole::LineKind::Notice, echo.c_str());
                    return Result::Line;
                }

                case VK_BACK:
                    if (ctrl)
                    {
                        const size_t to = PrevWord(s_caret);
                        s_buffer.erase(to, s_caret - to);
                        s_caret = to;
                    }
                    else if (s_caret > 0)
                    {
                        s_buffer.erase(s_caret - 1, 1);
                        --s_caret;
                    }
                    Refresh();
                    break;

                case VK_DELETE:
                    if (s_caret < s_buffer.size())
                    {
                        s_buffer.erase(s_caret, 1);
                        Refresh();
                    }
                    break;

                case VK_LEFT:
                    if (s_caret > 0)
                    {
                        s_caret = ctrl ? PrevWord(s_caret) : s_caret - 1;
                        Refresh();
                    }
                    break;

                case VK_RIGHT:
                    if (s_caret < s_buffer.size())
                    {
                        s_caret = ctrl ? NextWord(s_caret) : s_caret + 1;
                        Refresh();
                    }
                    break;

                case VK_HOME:
                    s_caret = 0;
                    Refresh();
                    break;

                case VK_END:
                    s_caret = s_buffer.size();
                    Refresh();
                    break;

                case VK_UP:
                    if (!s_history.empty())
                    {
                        if (s_historyPos == -1)
                        {
                            s_stashed    = s_buffer;
                            s_historyPos = static_cast<int>(s_history.size());
                        }
                        if (s_historyPos > 0)
                            LoadHistory(s_historyPos - 1);
                    }
                    break;

                case VK_DOWN:
                    if (s_historyPos >= 0)
                    {
                        const int next = s_historyPos + 1;
                        LoadHistory(next >= static_cast<int>(s_history.size()) ? -1 : next);
                    }
                    break;

                case VK_TAB:
                    Complete();
                    break;

                case VK_ESCAPE:
                    s_buffer.clear();
                    s_caret      = 0;
                    s_historyPos = -1;
                    Refresh();
                    break;

                default:
                {
                    // Ctrl+C with no selection (QuickEdit handles the selection
                    // case itself) and Ctrl+U both mean "forget this line".
                    if (ctrl && (key.wVirtualKeyCode == 'C' || key.wVirtualKeyCode == 'U'))
                    {
                        s_buffer.clear();
                        s_caret      = 0;
                        s_historyPos = -1;
                        Refresh();
                        break;
                    }

                    const wchar_t ch = key.uChar.UnicodeChar;
                    if (!ctrl && ch >= 0x20 && ch < 0x7F)
                    {
                        s_buffer.insert(s_caret, 1, static_cast<char>(ch));
                        ++s_caret;
                        Refresh();
                    }
                    break;
                }
                }
            }
        }
    }
}
