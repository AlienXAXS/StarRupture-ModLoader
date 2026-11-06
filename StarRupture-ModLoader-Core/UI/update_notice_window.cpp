#include "pch.h"
#include "update_notice_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include <algorithm>
#include <functional>
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
        std::vector<std::string> notes; // one entry per line; empty when the manifest had none
    };

    // s_mutex guards s_updates: AddUpdatedPlugin runs on the init thread
    // while Render/ShowIfPending run on the render / game threads.
    static std::mutex                 s_mutex;
    static std::vector<UpdatedPlugin> s_updates;
    static std::function<void()>      s_onShown;
    static bool s_isOpen = false;
    static bool s_shown  = false;   // one-shot: only auto-open once per session
    static int  s_centerFrames = 0; // frames left to keep the window centered while it sizes itself

    void AddUpdatedPlugin(const char* name, const char* oldVersion, const char* newVersion,
                          const char* notes)
    {
        if (!name || !name[0]) return;

        std::lock_guard<std::mutex> lock(s_mutex);
        UpdatedPlugin u;
        u.name       = name;
        u.oldVersion = oldVersion ? oldVersion : "";
        u.newVersion = newVersion ? newVersion : "";

        const std::string text = notes ? notes : "";
        for (size_t pos = 0; pos < text.size();)
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // Release lines carry their own "- "; the window draws the bullet.
            // A "## <version>" line is a group header and is kept whole.
            if (line.compare(0, 3, "## ") != 0 && line.compare(0, 2, "- ") == 0)
                line.erase(0, 2);
            if (!line.empty())
                u.notes.push_back(std::move(line));
            pos = end + 1;
        }
        s_updates.push_back(std::move(u));
    }

    void SetOnShown(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_onShown = std::move(fn);
    }

    void ShowIfPending()
    {
        if (s_shown) return;

        std::function<void()> onShown;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_updates.empty()) return;

            s_isOpen = true;
            s_shown  = true;
            s_centerFrames = 3;
            onShown = std::move(s_onShown);
            s_onShown = nullptr;
        }
        if (onShown)
            onShown();
    }

    void Render()
    {
        if (!s_isOpen)
            return;

        ImGuiIO& io = ImGui::GetIO();
        // Width follows the screen (35% of it, never under 640) so the notes
        // stay readable at 4K, where a fixed 640 is a thin column. It is still
        // fixed for the frame, not content-driven: the centered Close button
        // below offsets itself from the window width, so AlwaysAutoResize would
        // feed back into itself and grow the window every frame. Height is
        // auto-fit (0 = auto on that axis).
        const float width = std::max(640.0f, io.DisplaySize.x * 0.35f);
        ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
        // The list child sizes itself to its notes over the first frames, so
        // centering once on appearing would centre the window at its first
        // (empty) height. Re-centre for those frames, then leave it movable.
        if (s_centerFrames > 0)
        {
            --s_centerFrames;
            ImGui::SetNextWindowPos(
                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                ImGuiCond_Always,
                ImVec2(0.5f, 0.5f));
        }

        if (!UI::Theme::BeginChamferedWindow("Plugin Updates##update_notice",
                                             "PLUGINS UPDATED", &s_isOpen,
                                             nullptr,
                                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                                             ImGuiWindowFlags_NoScrollWithMouse))
            return;

        ImGui::TextUnformatted("The following plugins were updated automatically:");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Notes can make the list taller than the screen. Only the list scrolls,
        // in a child that fits its content up to what is left of 80% of the
        // screen height once the title, the intro above and the Close button
        // below are taken out, so those stay in view at any list length.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float footerHeight = style.ItemSpacing.y * 3.0f + ImGui::GetFrameHeight() + style.WindowPadding.y;
        const float maxListHeight = io.DisplaySize.y * 0.8f - ImGui::GetCursorPosY() - footerHeight;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, maxListHeight));
        ImGui::BeginChild("##update_notice_list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            // Plugins whose notes are word for word the same (a shared "works
            // after a game update" line, say) share one entry that names them
            // all, so the same bullet is not repeated per plugin. Versions are
            // left out for a group; the Plugins tab shows them.
            const auto groups = GroupByNotes(s_updates, [](const UpdatedPlugin& u) -> const std::vector<std::string>& { return u.notes; });
            for (const auto& g : groups)
            {
                const UpdatedPlugin& u = *g.front();
                ImGui::Bullet();
                ImGui::SameLine();
                if (g.size() == 1)
                {
                    ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "%s", u.name.c_str());
                    ImGui::SameLine();
                    if (!u.oldVersion.empty())
                        ImGui::TextDisabled("%s -> %s", u.oldVersion.c_str(), u.newVersion.c_str());
                    else
                        ImGui::TextDisabled("installed %s", u.newVersion.c_str());
                }
                else
                {
                    std::string names;
                    for (const UpdatedPlugin* p : g)
                        names += (names.empty() ? "" : ", ") + p->name;
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::AccentColorVec4(1.0f));
                    ImGui::TextWrapped("%s", names.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Indent();
                for (const std::string& line : u.notes)
                {
                    // "## v1.23.0": the version the lines below it belong to.
                    if (line.compare(0, 3, "## ") == 0)
                    {
                        ImGui::TextDisabled("%s", line.c_str() + 3);
                        continue;
                    }
                    ImGui::Bullet();
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", line.c_str());
                }
                ImGui::Unindent();
            }
        }
        ImGui::EndChild();

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

    bool IsOpen()
    {
        return s_isOpen;
    }

#ifdef _DEBUG
    void PopulateTestData()
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_updates.clear();
            s_updates.push_back({ "ServerUtility",   "1.4.2", "1.5.0", { "Fixed a crash when changing servers" } });
            s_updates.push_back({ "MapTweaks",       "0.9.0", "1.0.0", {} });
            s_updates.push_back({ "CraftingHelper",  "",      "2.1.3", {} });
        }
        s_isOpen = true;
        s_shown  = true;
        s_centerFrames = 3;
    }
#endif
}

#endif // MODLOADER_CLIENT_BUILD
