#include "pch.h"
#include "plugin_scan_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include "hook_failure_window.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace UI::PluginScanWindow
{
    // s_mutex guards s_posted and the write side of s_hasPosted: Show() runs on
    // the game thread while Render() runs on the render thread. The flag is
    // atomic only so Render() can skip the lock on the frames with nothing new.
    // Everything else is render-thread only.
    static std::mutex                                s_mutex;
    static std::vector<PluginManager::ScanResult>    s_posted;
    static std::atomic<bool>                         s_hasPosted{false};

    static std::vector<PluginManager::ScanResult>    s_results;   // what the window shows
    static bool s_isOpen = false;

    // The plugin list's status colours.
    static ImVec4 StatusColor(PluginManager::ScanStatus status)
    {
        switch (status)
        {
        case PluginManager::ScanStatus::Live:
            return ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
        case PluginManager::ScanStatus::Late:
        case PluginManager::ScanStatus::NeedsRestart:
        case PluginManager::ScanStatus::Retry:
        case PluginManager::ScanStatus::Refused:
            return ImVec4(1.0f, 0.5f, 0.1f, 1.0f);
        default:
            return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        }
    }

    void Show(std::vector<PluginManager::ScanResult> results)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_posted = std::move(results);
        s_hasPosted = true;
    }

    void Render()
    {
        // Picked up here rather than in Show() so the open state is only ever
        // touched on the render thread.
        if (s_hasPosted)
        {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_results = std::move(s_posted);
                s_hasPosted = false;
            }
            s_isOpen = std::any_of(s_results.begin(), s_results.end(),
                [](const PluginManager::ScanResult& r) { return r.status != PluginManager::ScanStatus::Live; });
        }

        if (!s_isOpen)
            return;

        ImGuiIO& io = ImGui::GetIO();
        // Fixed width, auto-fit height, for the same reason as UpdateNoticeWindow.
        ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(640, 0), ImVec2(640, io.DisplaySize.y * 0.8f));
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Appearing,
            ImVec2(0.5f, 0.5f));

        if (!UI::Theme::BeginChamferedWindow("New Plugins##plugin_scan",
                                             "NEW PLUGINS", &s_isOpen,
                                             nullptr,
                                             ImGuiWindowFlags_NoResize))
            return;

        ImGui::TextUnformatted("LOAD NEW found these plugin DLLs:");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        for (size_t i = 0; i < s_results.size(); ++i)
        {
            const PluginManager::ScanResult& r = s_results[i];

            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "%s", r.name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(StatusColor(r.status), "%s", PluginManager::ScanStatusLabel(r.status));
            if (r.fileName != r.name)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", r.fileName.c_str());
            }

            ImGui::Indent();
            ImGui::TextWrapped("%s", r.action.c_str());
            if (r.status == PluginManager::ScanStatus::HooksFailed && ImGui::Button("WHY?"))
                UI::HookFailureWindow::Show();
            ImGui::Unindent();
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Close"))
            s_isOpen = false;

        UI::Theme::EndChamferedWindow();
    }

    bool IsOpen()
    {
        return s_isOpen;
    }
}

#endif // MODLOADER_CLIENT_BUILD
