#include "pch.h"
#include "update_notice_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include <mutex>
#include <string>
#include <vector>

namespace UI::UpdateNoticeWindow
{
    struct UpdatedPlugin
    {
        std::string name;
        std::string oldVersion; // empty when no previous version was stored
        std::string newVersion;
    };

    // s_mutex guards s_updates: AddUpdatedPlugin runs on the init thread
    // while Render/ShowIfPending run on the render / game threads.
    static std::mutex                 s_mutex;
    static std::vector<UpdatedPlugin> s_updates;
    static bool s_isOpen = false;
    static bool s_shown  = false;   // one-shot: only auto-open once per session

    void AddUpdatedPlugin(const char* name, const char* oldVersion, const char* newVersion)
    {
        if (!name || !name[0]) return;

        std::lock_guard<std::mutex> lock(s_mutex);
        UpdatedPlugin u;
        u.name       = name;
        u.oldVersion = oldVersion ? oldVersion : "";
        u.newVersion = newVersion ? newVersion : "";
        s_updates.push_back(std::move(u));
    }

    void ShowIfPending()
    {
        if (s_shown) return;

        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_updates.empty()) return;

        s_isOpen = true;
        s_shown  = true;
    }

    void Render()
    {
        if (!s_isOpen)
            return;

        ImGuiIO& io = ImGui::GetIO();
        // Fixed width, auto-fit height (0 = auto on that axis). Width must not
        // be content-driven: the centered Close button below offsets itself from
        // the window width, so AlwaysAutoResize would feed back into itself and
        // grow the window every frame.
        ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Appearing,
            ImVec2(0.5f, 0.5f));

        if (!UI::Theme::BeginChamferedWindow("Plugin Updates##update_notice",
                                             "PLUGINS UPDATED", &s_isOpen,
                                             nullptr,
                                             ImGuiWindowFlags_NoResize))
            return;

        ImGui::TextUnformatted("The following plugins were updated automatically\n"
                               "when the mod loader started:");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (const UpdatedPlugin& u : s_updates)
            {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "%s", u.name.c_str());
                ImGui::SameLine();
                if (!u.oldVersion.empty())
                    ImGui::TextDisabled("%s -> %s", u.oldVersion.c_str(), u.newVersion.c_str());
                else
                    ImGui::TextDisabled("installed %s", u.newVersion.c_str());
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Centered Close button at the bottom.
        const float buttonWidth = 120.0f;
        float offset = (ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        if (ImGui::Button("Close", ImVec2(buttonWidth, 0.0f)))
            s_isOpen = false;

        UI::Theme::EndChamferedWindow();
    }

#ifdef _DEBUG
    void PopulateTestData()
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_updates.clear();
            s_updates.push_back({ "ServerUtility",   "1.4.2", "1.5.0" });
            s_updates.push_back({ "MapTweaks",       "0.9.0", "1.0.0" });
            s_updates.push_back({ "CraftingHelper",  "",      "2.1.3" });
        }
        s_isOpen = true;
        s_shown  = true;
    }
#endif
}

#endif // MODLOADER_CLIENT_BUILD
