#include "pch.h"
#include "hook_failure_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include "plugins/plugin_hook_report.h"

#include <string>
#include <vector>

namespace UI::HookFailureWindow
{
    namespace
    {
        // Display copy of the report, rebuilt only when PluginHookReport's
        // generation moves. Snapshot() copies every string in every failure, so
        // doing it per frame to render a window that changes once a session
        // would be pure waste -- and a reload of a plugin already on screen is
        // exactly the change a "did the selection change?" test would miss.
        std::vector<PluginHookReport::PluginReport> s_cache;
        unsigned s_cacheGeneration = 0;
        bool     s_cacheValid      = false;

        bool  s_isOpen      = false;
        bool  s_shown       = false;   // one-shot: only auto-open once per session
        float s_copiedUntil = 0.0f;    // ImGui::GetTime() the "Copied" label expires

        // Details are assembled with CRLF so the clipboard text pastes correctly
        // into Windows editors; ImGui has no use for the CR, so strip it here
        // rather than storing two versions of every string.
        std::string ForDisplay(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s)
                if (c != '\r') out += c;
            return out;
        }

        void RefreshCache()
        {
            const unsigned generation = PluginHookReport::GetGeneration();
            if (s_cacheValid && generation == s_cacheGeneration)
                return;

            s_cache           = PluginHookReport::Snapshot();
            s_cacheGeneration = generation;
            s_cacheValid      = true;
        }

        const ImVec4 kRed    = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        const ImVec4 kOrange = ImVec4(1.0f, 0.65f, 0.20f, 1.0f);
    }

    void Show()
    {
        s_isOpen = true;
        s_shown  = true;
        s_cacheValid = false;
    }

    void ShowIfPending()
    {
        if (s_shown) return;
        if (PluginHookReport::GetReportedPluginCount() == 0) return;
        Show();
    }

    void Render()
    {
        if (!s_isOpen)
            return;

        RefreshCache();

        ImGuiIO& io = ImGui::GetIO();

        // Fixed width for the same reason UpdateNoticeWindow has one: the
        // buttons at the bottom offset themselves from the window width, so an
        // auto-resizing width would feed back into itself every frame.
        ImGui::SetNextWindowSize(ImVec2(760.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Appearing,
            ImVec2(0.5f, 0.5f));

        if (!UI::Theme::BeginChamferedWindow("Plugin Hooks##hook_failures",
                                             "PLUGIN HOOKS FAILED", &s_isOpen,
                                             nullptr,
                                             ImGuiWindowFlags_NoResize))
            return;

        int refused = 0;
        for (const auto& r : s_cache)
            if (r.refused) ++refused;

        if (refused > 0)
            ImGui::TextColored(kRed, "%d plugin(s) did not load.", refused);
        ImGui::TextWrapped(
            "These plugins could not find the game code they hook into. That normally means "
            "the game updated and the plugin needs a new build -- it is not something you can "
            "fix from here. Copy the details below and send them to the plugin's author.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Height-capped scroll region: a report is usually two or three lines,
        // but a plugin resolving twenty patterns against a changed build can
        // produce a list taller than the screen.
        const float maxListHeight = io.DisplaySize.y * 0.5f;
        ImGui::BeginChild("##hook_failure_list", ImVec2(0.0f, maxListHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        for (size_t i = 0; i < s_cache.size(); ++i)
        {
            const PluginHookReport::PluginReport& r = s_cache[i];

            if (i > 0)
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "%s", r.plugin.c_str());
            ImGui::SameLine();
            if (r.refused)
                ImGui::TextColored(kRed, "NOT LOADED");
            else
                ImGui::TextColored(kOrange, "loaded with warnings");

            ImGui::TextDisabled("%s  --  %d pattern(s) resolved, %d failed",
                                r.file.empty() ? "?" : r.file.c_str(),
                                r.resolved, static_cast<int>(r.failures.size()));

            ImGui::Spacing();
            ImGui::Indent();
            for (const PluginHookReport::Failure& f : r.failures)
            {
                if (f.fatal)
                    ImGui::TextColored(kRed, "[required]");
                else
                    ImGui::TextColored(kOrange, "[optional]");
                ImGui::SameLine();
                ImGui::TextUnformatted(f.hookName.c_str());

                ImGui::Indent();
                // Not wrapped: an AOB pattern broken mid-byte is harder to read
                // than one that runs off the side of a scrollable region.
                ImGui::TextDisabled("%s", ForDisplay(f.detail).c_str());
                ImGui::Unindent();
            }
            ImGui::Unindent();
        }

        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonWidth = 150.0f;
        const float spacing     = ImGui::GetStyle().ItemSpacing.x;
        float offset = (ImGui::GetContentRegionAvail().x - (buttonWidth * 2.0f + spacing)) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button("Copy Details", ImVec2(buttonWidth, 0.0f)))
        {
            // Built fresh rather than from the display cache: the clipboard text
            // carries the loader and game build too, which is the first thing an
            // author needs and the one thing the window itself does not show.
            const std::string text = PluginHookReport::BuildReportText();
            ImGui::SetClipboardText(text.c_str());
            s_copiedUntil = static_cast<float>(ImGui::GetTime()) + 2.0f;
        }

        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(buttonWidth, 0.0f)))
            s_isOpen = false;

        if (static_cast<float>(ImGui::GetTime()) < s_copiedUntil)
        {
            ImGui::SameLine();
            ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "Copied");
        }

        UI::Theme::EndChamferedWindow();
    }

    bool IsOpen()
    {
        return s_isOpen;
    }

#ifdef _DEBUG
    void PopulateTestData()
    {
        PluginHookReport::PluginReport refusedPlugin;
        refusedPlugin.plugin   = "CraftingHelper";
        refusedPlugin.file     = "CraftingHelper.dll";
        refusedPlugin.refused  = true;
        refusedPlugin.resolved = 3;
        refusedPlugin.failures.push_back({ "UCrCraftingComponent::FinishCrafting",
            "pattern not found: 40 55 53 56 57 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ??", true });
        refusedPlugin.failures.push_back({ "UCrCraftingComponent::CanCraft",
            "pattern not found: 48 89 5C 24 ?? 57 48 83 EC 20 48 8B D9", true });

        PluginHookReport::PluginReport warnPlugin;
        warnPlugin.plugin   = "MapTweaks";
        warnPlugin.file     = "MapTweaks.dll";
        warnPlugin.refused  = false;
        warnPlugin.resolved = 7;
        warnPlugin.failures.push_back({ "ACrMapActor::RefreshFogOfWar",
            "pattern not found: E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B CB", false });

        PluginHookReport::InjectTestReport(refusedPlugin);
        PluginHookReport::InjectTestReport(warnPlugin);
        Show();
    }
#endif
}

#endif // MODLOADER_CLIENT_BUILD
