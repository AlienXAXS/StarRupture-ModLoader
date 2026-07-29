#include "pch.h"
#include "console_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include "logging/logger.h"
#include "hooks/game/console_command/console_command.h"
#include "hooks/input/keybind_registry.h"
#include "utils/game_thread_dispatch.h"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace UI::ConsoleWindow
{
    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    enum class LineKind { Command, Output, Notice, Error };

    struct Line
    {
        LineKind    kind;
        std::string text;
    };

    static bool        s_isOpen      = false;
    static bool        s_focusInput  = false;   // grab focus on the frame after opening
    static EModKey     s_openKey     = EModKey::Tilde;
    static char        s_openKeyName[32] = "Tilde";
    static wchar_t     s_iniPath[MAX_PATH] = {};

    // s_lines is appended from the game thread (command results arrive via
    // GameThreadDispatch) and read from the render thread, so it needs a lock.
    // Everything else here is render-thread only.
    static std::mutex               s_linesMutex;
    static std::vector<Line>        s_lines;

    static std::vector<std::string> s_history;
    static int                      s_historyPos = -1;   // -1 == not navigating
    static char                     s_input[512] = {};
    static bool                     s_scrollToBottom = false;

    // Keep the scrollback bounded -- a chatty command can dump a lot.
    static constexpr size_t kMaxLines = 2000;

    // Fraction of the viewport height the console occupies, UE-style.
    static constexpr float kHeightFraction = 0.42f;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    static void AddLine(LineKind kind, std::string text)
    {
        std::lock_guard<std::mutex> lk(s_linesMutex);
        s_lines.push_back({ kind, std::move(text) });
        if (s_lines.size() > kMaxLines)
            s_lines.erase(s_lines.begin(), s_lines.begin() + (s_lines.size() - kMaxLines));
        s_scrollToBottom = true;
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), len, nullptr, nullptr);
        return out;
    }

    static std::wstring Utf8ToWide(const char* s)
    {
        if (!s || !*s) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    // Engine output arrives as one blob; split so long results scroll sensibly.
    static void AddOutputBlob(const std::wstring& blob)
    {
        const std::string utf8 = WideToUtf8(blob);
        size_t start = 0;
        while (start <= utf8.size())
        {
            size_t nl = utf8.find('\n', start);
            if (nl == std::string::npos) nl = utf8.size();

            std::string line = utf8.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (!line.empty())
                AddLine(LineKind::Output, std::move(line));

            if (nl == utf8.size()) break;
            start = nl + 1;
        }
    }

    static void Submit(const char* cmdRaw)
    {
        // Trim leading/trailing whitespace
        std::string cmd(cmdRaw ? cmdRaw : "");
        const size_t b = cmd.find_first_not_of(" \t");
        if (b == std::string::npos) return;
        const size_t e = cmd.find_last_not_of(" \t");
        cmd = cmd.substr(b, e - b + 1);
        if (cmd.empty()) return;

        AddLine(LineKind::Command, "> " + cmd);

        // Keep history unique-adjacent and most-recent-last.
        if (s_history.empty() || s_history.back() != cmd)
            s_history.push_back(cmd);
        s_historyPos = -1;

        // Local commands handled without touching the engine.
        if (_stricmp(cmd.c_str(), "clear") == 0 || _stricmp(cmd.c_str(), "cls") == 0)
        {
            std::lock_guard<std::mutex> lk(s_linesMutex);
            s_lines.clear();
            return;
        }

        // We are on the render thread here (ImGui draws from the Present hook),
        // but APlayerController::ConsoleCommand walks engine state and trips
        // check(IsInGameThread()). Hand it to the game thread and let the
        // result come back asynchronously.
        GameThreadDispatch::PostVoid([cmd]()
        {
            std::wstring output;
            if (!Hooks::ConsoleCommand::Execute(Utf8ToWide(cmd.c_str()).c_str(), output))
            {
                AddLine(LineKind::Error,
                        "Command could not be executed (no player controller, or "
                        "APlayerController::ConsoleCommand was not resolved). See modloader.log.");
                return;
            }

            if (!output.empty())
                AddOutputBlob(output);
        });
    }

    // Up/Down history navigation for the input box.
    static int InputCallback(ImGuiInputTextCallbackData* data)
    {
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || s_history.empty())
            return 0;

        const int prev = s_historyPos;

        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (s_historyPos == -1)
                s_historyPos = static_cast<int>(s_history.size()) - 1;
            else if (s_historyPos > 0)
                --s_historyPos;
        }
        else if (data->EventKey == ImGuiKey_DownArrow)
        {
            if (s_historyPos != -1)
            {
                if (++s_historyPos >= static_cast<int>(s_history.size()))
                    s_historyPos = -1;
            }
        }

        if (prev != s_historyPos)
        {
            const char* replacement = (s_historyPos >= 0) ? s_history[s_historyPos].c_str() : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, replacement);
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------
    bool IsOpen() { return s_isOpen; }

    const char* GetOpenKeyName() { return s_openKeyName; }

    void Open()
    {
        s_isOpen     = true;
        s_focusInput = true;

        // While we are open the prompt holds keyboard focus, which normally
        // makes the keybind registry swallow the open key so it can be typed.
        // Exempt it for the lifetime of the window so it still closes us.
        Hooks::Input::SetTypingExempt(s_openKey, true);
    }

    void Close()
    {
        s_isOpen = false;

        // Discard whatever was half-typed, and hand the key back to normal
        // typing behaviour everywhere else.
        s_input[0]   = '\0';
        s_historyPos = -1;
        Hooks::Input::SetTypingExempt(s_openKey, false);
    }

    void Toggle()
    {
        ModLoaderLogger::LogDebug(L"[Console] Open key fired (open=%d)", s_isOpen ? 1 : 0);
        if (s_isOpen) Close();
        else          Open();
    }

    void Load(const wchar_t* iniPath)
    {
        if (iniPath)
            wcsncpy_s(s_iniPath, iniPath, _TRUNCATE);

        wchar_t keyW[32] = {};
        GetPrivateProfileStringW(L"Console", L"OpenKey", L"Tilde", keyW, 32, s_iniPath);
        char keyBuf[32] = {};
        for (int i = 0; i < 31 && keyW[i]; ++i)
            keyBuf[i] = static_cast<char>(keyW[i]);

        EModKey key = Hooks::Input::NameToModKey(keyBuf);
        if (key == EModKey::Unknown)
        {
            key = EModKey::Tilde;
            strcpy_s(keyBuf, "Tilde");
        }
        s_openKey = key;
        strncpy_s(s_openKeyName, keyBuf, _TRUNCATE);

        // Registered unconditionally; the callback is inert while disabled, so
        // toggling the setting takes effect without re-registering.
        Hooks::Input::RegisterKeybind(s_openKey, EModKeyEvent::Pressed,
            [](EModKey, EModKeyEvent) { UI::ConsoleWindow::Toggle(); });

        ModLoaderLogger::LogInfo(
            L"[Console] Developer console ready, open key '%S' (EModKey=%d, VK=0x%02X)",
            s_openKeyName,
            static_cast<int>(s_openKey),
            Hooks::Input::ModKeyToVK(s_openKey));

        Hooks::ConsoleCommand::Resolve();
    }

    // -----------------------------------------------------------------------
    // Render -- UE-style: pinned to the top of the viewport, full width.
    // -----------------------------------------------------------------------
    void Render()
    {
        if (!IsOpen())
            return;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float height = vp->Size.y * kHeightFraction;

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, height));
        ImGui::SetNextWindowBgAlpha(0.88f);

        const ImGuiWindowFlags winFlags =
            ImGuiWindowFlags_NoTitleBar   |
            ImGuiWindowFlags_NoResize     |
            ImGuiWindowFlags_NoMove       |
            ImGuiWindowFlags_NoCollapse   |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.03f, 1.0f));

        if (!ImGui::Begin("##devconsole", nullptr, winFlags))
        {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            return;
        }

        // Header strip: title on the left, hint + close on the right.
        ImGui::TextDisabled("Developer Console");
        ImGui::SameLine();
        ImGui::TextDisabled("   %s to close   |   Up/Down for history   |   'clear' to wipe",
                            s_openKeyName);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50.0f);
        if (ImGui::SmallButton("Close"))
            Close();

        ImGui::Separator();

        // Reserve a row for the prompt line.
        const float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();

        if (ImGui::BeginChild("##scrollback", ImVec2(0.0f, -footer), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
            {
                std::lock_guard<std::mutex> lk(s_linesMutex);
                for (const Line& line : s_lines)
                {
                    ImVec4 col;
                    switch (line.kind)
                    {
                    case LineKind::Command: col = ImVec4(0.55f, 0.78f, 1.00f, 1.0f); break;
                    case LineKind::Error:   col = ImVec4(1.00f, 0.42f, 0.38f, 1.0f); break;
                    case LineKind::Notice:  col = ImVec4(0.70f, 0.70f, 0.70f, 1.0f); break;
                    default:                col = ImVec4(0.86f, 0.86f, 0.86f, 1.0f); break;
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopStyleVar();

            if (s_scrollToBottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            s_scrollToBottom = false;
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Prompt line
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(">");
        ImGui::SameLine();

        const ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackHistory;

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##cmd", s_input, sizeof(s_input), flags, &InputCallback))
        {
            Submit(s_input);
            s_input[0]   = '\0';
            s_focusInput = true;
        }

        // Keep focus in the box after opening and after each submit.
        if (s_focusInput)
        {
            ImGui::SetKeyboardFocusHere(-1);
            s_focusInput = false;
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

#endif // MODLOADER_CLIENT_BUILD
