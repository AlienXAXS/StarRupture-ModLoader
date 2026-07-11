#include "pch.h"
#include "tick_profiler_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../hooks/game/engine_tick/tick_profiler.h"
#include "logging/logger.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace UI::TickProfilerWindow
{
    static bool s_isOpen = false;

    void Open()   { s_isOpen = true; }
    void Toggle() { s_isOpen = !s_isOpen; }
    bool IsOpen() { return s_isOpen; }

    static void RenderControls()
    {
        bool logStutters = Hooks::EngineTick::IsStutterLoggingEnabled();
        if (UI::Theme::ToggleSwitch("Log Tick Stutters", &logStutters))
            Hooks::EngineTick::SetStutterLoggingEnabled(logStutters);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Logs a warning to modloader.log when a game-thread tick takes much longer\n"
                               "than the recent average, while ChimeraMain is ticking. Runtime-only, not persisted.");

        ImGui::SameLine(0.0f, 32.0f);

        bool sampleStacks = Hooks::EngineTick::IsStackSamplingEnabled();
        if (UI::Theme::ToggleSwitch("Stack Sample Stutters", &sampleStacks))
            Hooks::EngineTick::SetStackSamplingEnabled(sampleStacks);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Heavier: repeatedly suspends/resumes the game thread to sample its call\n"
                               "stack during the engine tick, so stutters show which functions were hot.\n"
                               "Resolves to ClassName::FunctionName where a native UFUNCTION matched,\n"
                               "Module+Offset otherwise. Requires Log Tick Stutters. Runtime-only.");
    }

    static void RenderLiveGraph()
    {
        std::vector<Hooks::EngineTick::TickProfiler::TickSample> samples =
            Hooks::EngineTick::TickProfiler::GetLiveSamples();

        if (samples.empty())
        {
            ImGui::TextDisabled("No tick data yet -- waiting for ChimeraMain to be ticking...");
            return;
        }

        std::vector<float> values;
        values.reserve(samples.size());
        float maxMs = 1.0f;
        for (const Hooks::EngineTick::TickProfiler::TickSample& s : samples)
        {
            const float ms = static_cast<float>(s.totalMs);
            values.push_back(ms);
            maxMs = std::max(maxMs, ms);
        }

        char overlay[64];
        snprintf(overlay, sizeof(overlay), "latest: %.2fms", values.back());

        ImGui::PlotLines("##ticktimes", values.data(), static_cast<int>(values.size()),
                          0, overlay, 0.0f, maxMs * 1.1f, ImVec2(-1, 120));

        ImGui::TextDisabled("Tick #%llu  |  %.2fms  |  %zu samples",
                             static_cast<unsigned long long>(samples.back().tickIndex),
                             samples.back().totalMs, samples.size());
    }

    // Root/outermost captured caller at the top, drilling down to the leaf
    // -- real merged data (every node's count is samples that actually
    // passed through it), not one representative example.
    static void RenderCallTreeNode(const Hooks::EngineTick::TickProfiler::CallTreeNode& node, size_t totalSamples)
    {
        const double pct = totalSamples > 0
            ? (100.0 * node.sampleCount / static_cast<double>(totalSamples))
            : 0.0;

        char label[224];
        if (!node.symbolName.empty())
            snprintf(label, sizeof(label), "%s -- %d (%.1f%%)##ct%llX",
                      node.symbolName.c_str(), node.sampleCount, pct,
                      static_cast<unsigned long long>(node.functionStart));
        else
            snprintf(label, sizeof(label), "%s+0x%llX -- %d (%.1f%%)##ct%llX",
                      node.module.c_str(), static_cast<unsigned long long>(node.offset),
                      node.sampleCount, pct, static_cast<unsigned long long>(node.functionStart));

        if (node.children.empty())
        {
            ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
                                       ImGuiTreeNodeFlags_NoTreePushOnOpen);
            return;
        }

        // Single-child chains (the common case -- one hot path down to a
        // leaf) start expanded so the interesting part isn't hidden behind
        // N clicks; branches (>1 child) start collapsed to keep the initial
        // view scannable.
        ImGuiTreeNodeFlags flags = node.children.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        if (ImGui::TreeNodeEx(label, flags))
        {
            for (const Hooks::EngineTick::TickProfiler::CallTreeNode& child : node.children)
                RenderCallTreeNode(child, totalSamples);
            ImGui::TreePop();
        }
    }

    // ASCII only (no box-drawing Unicode) per project convention -- see
    // CLAUDE.md. Standard tree layout: "+-- " for a node with siblings
    // after it, "\-- " for the last child, "|   "/"    " continuing the
    // prefix for their descendants.
    static void AppendCallTreeAscii(const Hooks::EngineTick::TickProfiler::CallTreeNode& node,
        size_t totalSamples, const std::string& prefix, bool isLast, bool isRoot,
        std::vector<std::string>& outLines)
    {
        const double pct = totalSamples > 0
            ? (100.0 * node.sampleCount / static_cast<double>(totalSamples))
            : 0.0;

        char nameBuf[224];
        if (!node.symbolName.empty())
            snprintf(nameBuf, sizeof(nameBuf), "%s -- %d (%.1f%%)",
                      node.symbolName.c_str(), node.sampleCount, pct);
        else
            snprintf(nameBuf, sizeof(nameBuf), "%s+0x%llX -- %d (%.1f%%)",
                      node.module.c_str(), static_cast<unsigned long long>(node.offset),
                      node.sampleCount, pct);

        std::string line = prefix;
        if (!isRoot)
            line += isLast ? "\\-- " : "+-- ";
        line += nameBuf;
        outLines.push_back(std::move(line));

        std::string childPrefix = prefix;
        if (!isRoot)
            childPrefix += isLast ? "    " : "|   ";

        for (size_t i = 0; i < node.children.size(); ++i)
            AppendCallTreeAscii(node.children[i], totalSamples, childPrefix,
                                 i + 1 == node.children.size(), false, outLines);
    }

    // Dumps everything shown in one stutter's expanded row -- header,
    // segment breakdown, and (if stack sampling was on) the full call tree
    // as an ASCII tree -- to modloader.log, one ModLoaderLogger call per
    // line so it reads the same as the rest of the log.
    static void LogStutterEventToFile(const Hooks::EngineTick::TickProfiler::StutterEvent& ev)
    {
        ModLoaderLogger::LogInfo(L"[TickProfiler] ===== Stutter dump: Tick %llu @ %S =====",
                                  static_cast<unsigned long long>(ev.tickIndex), ev.timestamp.c_str());
        ModLoaderLogger::LogInfo(L"[TickProfiler] Total: %.2fms (avg %.2fms, delta %.4fs)",
                                  ev.totalMs, ev.avgMsAtTime, ev.deltaSeconds);

        ModLoaderLogger::LogInfo(L"[TickProfiler] Breakdown:");
        for (const Hooks::EngineTick::TickProfiler::SegmentTiming& seg : ev.breakdown)
        {
            const double pct = ev.totalMs > 0.0 ? (seg.ms / ev.totalMs) * 100.0 : 0.0;
            ModLoaderLogger::LogInfo(L"[TickProfiler]   %S: %.2fms (%.1f%%)", seg.name.c_str(), seg.ms, pct);
        }

        if (!ev.callTree.empty())
        {
            ModLoaderLogger::LogInfo(L"[TickProfiler] Call tree (root -> leaf), %zu samples during Engine Tick:",
                                      ev.totalSamples);

            std::vector<std::string> lines;
            for (const Hooks::EngineTick::TickProfiler::CallTreeNode& root : ev.callTree)
                AppendCallTreeAscii(root, ev.totalSamples, "", true, true, lines);

            for (const std::string& line : lines)
                ModLoaderLogger::LogInfo(L"[TickProfiler] %S", line.c_str());
        }
        else
        {
            ModLoaderLogger::LogInfo(L"[TickProfiler] (no call tree -- Stack Sample Stutters was off for this tick)");
        }

        ModLoaderLogger::LogInfo(L"[TickProfiler] ===== End dump =====");
    }

    static void RenderStutterList()
    {
        std::vector<Hooks::EngineTick::TickProfiler::StutterEvent> events =
            Hooks::EngineTick::TickProfiler::GetStutterEvents();

        ImGui::Text("Stutters recorded: %zu", events.size());
        ImGui::Spacing();

        if (events.empty())
        {
            ImGui::TextDisabled("None yet -- this fills up as ticks exceed the average threshold.");
            return;
        }

        // Fill whatever vertical space is left in the window instead of a
        // fixed height, so the list grows/shrinks as the window is resized.
        const float availY = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##stutter_scroll", ImVec2(0, availY > 100.0f ? availY : 100.0f), true);

        // Most recent first.
        for (auto it = events.rbegin(); it != events.rend(); ++it)
        {
            const Hooks::EngineTick::TickProfiler::StutterEvent& ev = *it;

            char label[192];
            snprintf(label, sizeof(label), "[%s] Tick %llu -- %.2fms (avg %.2fms)##%llu",
                      ev.timestamp.c_str(),
                      static_cast<unsigned long long>(ev.tickIndex),
                      ev.totalMs, ev.avgMsAtTime,
                      static_cast<unsigned long long>(ev.tickIndex));

            const bool isOpen = ImGui::TreeNode(label);

            char sendLogId[32];
            snprintf(sendLogId, sizeof(sendLogId), "Send To Log##sendlog%llu",
                      static_cast<unsigned long long>(ev.tickIndex));
            ImGui::SameLine();
            if (ImGui::SmallButton(sendLogId))
                LogStutterEventToFile(ev);

            if (isOpen)
            {
                ImGui::TextDisabled("Frame delta: %.4fs", ev.deltaSeconds);
                ImGui::Spacing();

                if (ImGui::BeginTable("##breakdown", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Segment");
                    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("% of tick", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();

                    for (const Hooks::EngineTick::TickProfiler::SegmentTiming& seg : ev.breakdown)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(seg.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", seg.ms);
                        ImGui::TableNextColumn();
                        const double pct = ev.totalMs > 0.0 ? (seg.ms / ev.totalMs) * 100.0 : 0.0;
                        ImGui::Text("%.1f%%", pct);
                    }

                    ImGui::EndTable();
                }

                if (!ev.callTree.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Call tree during Engine Tick: %zu samples (root -> ... -> leaf; named where a "
                                         "PDB symbol or native UFUNCTION matched, Module+Offset otherwise)", ev.totalSamples);
                    ImGui::Spacing();

                    for (const Hooks::EngineTick::TickProfiler::CallTreeNode& root : ev.callTree)
                        RenderCallTreeNode(root, ev.totalSamples);
                }
                else if (Hooks::EngineTick::IsStackSamplingEnabled())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("No stack samples captured for this tick.");
                }

                ImGui::TreePop();
            }
        }

        ImGui::EndChild();
    }

    void Render()
    {
        if (!s_isOpen)
            return;

        ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);

        if (!UI::Theme::BeginChamferedWindow("Tick Profiler##tick_profiler",
                                             "TICK PROFILER", &s_isOpen,
                                             ""))
            return;

        RenderControls();
        ImGui::Spacing();

        if (!Hooks::EngineTick::IsStutterLoggingEnabled())
        {
            ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f),
                "Stutter logging is off -- flip it on above to start collecting data.");
            ImGui::Spacing();
        }

        ImGui::SeparatorText("Recent tick times");
        RenderLiveGraph();

        ImGui::Spacing();
        ImGui::SeparatorText("Stutter events");
        RenderStutterList();

        UI::Theme::EndChamferedWindow();
    }
}

#endif // MODLOADER_CLIENT_BUILD
