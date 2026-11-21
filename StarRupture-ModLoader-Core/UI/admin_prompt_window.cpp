#include "pch.h"
#include "admin_prompt_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include "network_channel/admin_prompt.h"

namespace UI::AdminPromptWindow
{
    namespace
    {
        // The text box: the longest code the server takes, plus the NUL. It is
        // wiped when the code is sent and whenever the window is not showing it,
        // so a typed code never outlives the prompt.
        char        s_code[HostRulesWire::kMaxAdminRCode + 1] = {};
        const char* s_failure = nullptr;   // why the last Send did not go out; a literal, or null
        bool        s_refocus = false;     // put the cursor back in the box for another try

        const ImVec4 kRed = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);

        void ClearCode()
        {
            SecureZeroMemory(s_code, sizeof(s_code));
            s_failure = nullptr;
        }

        // What the player needs to know when a Send did not go out. Ok has
        // nothing to say; NotOpen and RateLimited are a click that raced the
        // server or a second click, and are ignored.
        const char* FailureText(AdminPrompt::SubmitResult r)
        {
            switch (r)
            {
            case AdminPrompt::SubmitResult::Empty:       return "Type the code first.";
            case AdminPrompt::SubmitResult::TooLong:     return "That code is too long.";
            case AdminPrompt::SubmitResult::ControlChar: return "That code has a character that is not allowed.";
            case AdminPrompt::SubmitResult::SendFailed:  return "Could not send the code. Try again.";
            default:                                     return nullptr;
            }
        }

        void Send()
        {
            const AdminPrompt::SubmitResult r = AdminPrompt::Submit(s_code);
            if (r == AdminPrompt::SubmitResult::Ok)
            {
                ClearCode();
                s_refocus = true;
            }
            else
            {
                s_failure = FailureText(r);
            }
        }
    }

    void Render()
    {
        const AdminPrompt::Snapshot snap = AdminPrompt::GetSnapshot();

        // The text box is only on screen while the server is asking.
        if (!snap.open || snap.final)
        {
            if (s_code[0] != '\0' || s_failure)
                ClearCode();
            if (!snap.open)
                return;
        }

        ImGuiIO& io = ImGui::GetIO();

        // Fixed width, as the other loader pop-ups have: wrapped text needs one.
        ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Appearing,
            ImVec2(0.5f, 0.5f));

        bool open = true;
        if (!UI::Theme::BeginChamferedWindow("Server admin##admin_prompt", "SERVER ADMIN", &open,
                                             nullptr, ImGuiWindowFlags_NoResize))
            return;

        if (snap.final)
        {
            ImGui::TextWrapped("%s", snap.status.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK"))
                open = false;
        }
        else
        {
            ImGui::TextWrapped("%s", AdminPrompt::kPromptText);

            if (!snap.status.empty())
            {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", snap.status.c_str());
            }
            if (s_failure)
            {
                ImGui::Spacing();
                ImGui::TextColored(kRed, "%s", s_failure);
            }

            ImGui::Spacing();
            if (ImGui::IsWindowAppearing() || s_refocus)
            {
                ImGui::SetKeyboardFocusHere();
                s_refocus = false;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##admin_code", s_code, sizeof(s_code),
                                 ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue) &&
                s_code[0] != '\0')
                Send();

            ImGui::Spacing();
            ImGui::BeginDisabled(s_code[0] == '\0');
            if (ImGui::Button("Send"))
                Send();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Not me"))
                open = false;
        }

        UI::Theme::EndChamferedWindow();

        // Not me, the OK of a last line and the title bar's X all end the prompt.
        if (!open)
        {
            AdminPrompt::NotMe();
            ClearCode();
        }
    }

    bool IsOpen()
    {
        return AdminPrompt::GetSnapshot().open;
    }
}

#endif // MODLOADER_CLIENT_BUILD
