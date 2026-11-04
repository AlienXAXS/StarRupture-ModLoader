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

        bool        s_isOpen      = false;
        std::string s_scrollTo;          // plugin whose entry the next frame scrolls to; empty when none
        bool     s_armed       = false;  // main menu reached; auto-open is live
        unsigned s_lastCommit  = 0;      // PluginHookReport commit serial we have shown
        float    s_copiedUntil = 0.0f;   // ImGui::GetTime() the "Copied" label expires

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

        // Opens the window when a report has been added since the last one we
        // showed. Keyed on the commit serial rather than the generation: a
        // reload that now resolves cleanly still moves the generation (it drops
        // the plugin's old entry) and must not pop a window saying nothing.
        //
        // Dismissing leaves the serial where it is, so the window stays shut
        // until something new actually fails -- which is the whole difference
        // between this and the one-shot flag it replaced.
        void PollForNewReports()
        {
            if (!s_armed)
                return;

            const unsigned commit = PluginHookReport::GetCommitSerial();
            if (commit == s_lastCommit)
                return;

            s_lastCommit = commit;
            if (PluginHookReport::GetReportedPluginCount() == 0)
                return;

            Show();
        }

        const ImVec4 kRed    = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        const ImVec4 kOrange = ImVec4(1.0f, 0.65f, 0.20f, 1.0f);
    }

    void Show()
    {
        s_isOpen     = true;
        s_cacheValid = false;
    }

    void ShowFor(const char* plugin)
    {
        s_scrollTo = plugin ? plugin : "";
        Show();
    }

    void ShowIfPending()
    {
        // Arms the auto-open, nothing more: this runs on the game thread from
        // the world-begin-play callback, and the check itself belongs on the
        // render thread with everything else that touches the open state. The
        // first poll after this then opens the window for whatever failed
        // during startup, which is what it used to do directly.
        s_armed = true;
    }

    void Render()
    {
        PollForNewReports();

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

        int  refused        = 0;
        int  featuresOff    = 0;
        int  featurePlugins = 0;
        bool anyFailures    = false;
        for (const auto& r : s_cache)
        {
            if (r.refused) ++refused;
            if (!r.failures.empty()) anyFailures = true;
            if (!r.featuresOff.empty()) ++featurePlugins;
            featuresOff += static_cast<int>(r.featuresOff.size());
        }

        // Nothing failed to load when only features are off, so the title
        // should not say hooks failed.
        if (!UI::Theme::BeginChamferedWindow("Plugin Hooks##hook_failures",
                                             anyFailures ? "PLUGIN HOOKS FAILED" : "PLUGIN FEATURES OFF",
                                             &s_isOpen, nullptr,
                                             ImGuiWindowFlags_NoResize))
            return;

        // What a player needs first, in a line each: which feature is off in
        // which plugin. The report below is for the plugin's author.
        if (featuresOff > 0)
        {
            ImGui::TextColored(kOrange, "%d feature(s) are off in %d plugin(s).", featuresOff, featurePlugins);
            for (const auto& r : s_cache)
                for (const PluginHookReport::FeatureOff& f : r.featuresOff)
                    ImGui::TextWrapped("%s: %s is off -- %s.",
                                       r.plugin.c_str(), f.title.c_str(), f.reason.c_str());
            ImGui::TextDisabled("The rest of each plugin runs. Details are listed below.");

            if (anyFailures)
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }
        }

        if (refused > 0)
            ImGui::TextColored(kRed, "%d plugin(s) did not load.", refused);
        if (anyFailures)
            ImGui::TextWrapped(
                "These plugins could not find the game code they hook into, or found more than one "
                "candidate for it. A plugin that misses a hook it cannot run without is not loaded. "
                "A miss that only one feature needs turns that feature off, and the rest of the "
                "plugin runs. That normally means the game updated and the plugin needs a new "
                "build -- it is not something you can fix from here. Copy the details below and "
                "send them to the plugin's author.");

        // Preload plugins fail in exactly the same way and belong in the same
        // list, but the consequence is different enough to be worth one line:
        // the game already started, so nobody is waiting on a decision.
        bool anyPreload = false;
        for (const auto& r : s_cache)
            anyPreload = anyPreload || r.preload;

        if (anyPreload)
        {
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Entries marked [preload] are DLLs from ModLoader\\Preload, which run before the "
                "game starts. One failing never stops the game launching -- it simply did not "
                "get its turn.");
        }

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

            if (!s_scrollTo.empty() && _stricmp(r.plugin.c_str(), s_scrollTo.c_str()) == 0)
            {
                ImGui::SetScrollHereY(0.0f);
                s_scrollTo.clear();
            }

            if (r.preload)
            {
                ImGui::TextDisabled("[preload]");
                ImGui::SameLine();
            }
            ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "%s", r.plugin.c_str());
            ImGui::SameLine();
            if (r.refused)
                ImGui::TextColored(kRed, r.preload ? "DID NOT RUN" : "NOT LOADED");
            else
                ImGui::TextColored(kOrange, "loaded");

            ImGui::TextDisabled("%s  --  %d pattern(s) resolved, %d failed",
                                r.file.empty() ? "?" : r.file.c_str(),
                                r.resolved, static_cast<int>(r.failures.size()));

            ImGui::Spacing();
            ImGui::Indent();
            for (const PluginHookReport::Failure& f : r.failures)
            {
                // Both colours are a warning colour: the tag says how the
                // plugin declared the resolve, not how much it mattered in the
                // end -- either one is why the plugin is not loaded.
                if (f.required)
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

            for (const PluginHookReport::FeatureOff& f : r.featuresOff)
            {
                ImGui::TextColored(kOrange, "[feature off]");
                ImGui::SameLine();
                ImGui::TextUnformatted(f.title.c_str());

                ImGui::Indent();
                for (const PluginHookReport::FeatureMiss& m : f.misses)
                {
                    ImGui::TextUnformatted(m.key.c_str());
                    ImGui::TextDisabled("%s", ForDisplay(m.detail).c_str());
                }
                ImGui::Unindent();
            }
            ImGui::Unindent();
        }

        s_scrollTo.clear();   // a name with no entry must not wait for the next one
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
        warnPlugin.refused  = true;
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
