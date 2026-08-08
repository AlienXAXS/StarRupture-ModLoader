#include "pch.h"
#include "logging_tab.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "global_settings.h"
#include "theme.h"
#include "logging/log.h"
#include "logging/plugin_log_levels.h"
#include "plugins/plugin_manager.h"
#include "hooks/game/log_verbosity/log_verbosity.h"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace UI::LoggingTab
{
    // -----------------------------------------------------------------------
    // Level columns
    //
    // "Default" is a column like any other rather than a separate control, so
    // undoing an override is the same single click as setting one -- and the
    // column a plugin sits in reads as its answer to "how loud is this?".
    // -----------------------------------------------------------------------
    struct LevelColumn
    {
        const char* header;
        int         value;      // PluginLogLevels::kInherit, or a LogToFile::Level
        const char* tooltip;
    };

    static const LevelColumn s_columns[] =
    {
        { "Default", PluginLogLevels::kInherit,
          "Follow the ModLoader Log Level above.\nThis is where every plugin starts each launch." },
        { "Trace",   static_cast<int>(LogToFile::Level::Trace),
          "Everything the plugin emits." },
        { "Debug",   static_cast<int>(LogToFile::Level::Debug),
          "Debug and above -- drops Trace." },
        { "Info",    static_cast<int>(LogToFile::Level::Info),
          "Info and above -- drops Trace and Debug." },
        { "Warn",    static_cast<int>(LogToFile::Level::Warn),
          "Warnings and errors only." },
        { "Error",   static_cast<int>(LogToFile::Level::Error),
          "Errors only. The quietest this plugin can be made." },
    };
    static constexpr int kColumnCount = static_cast<int>(std::size(s_columns));

    // Sentinel for the "set all" row when plugins disagree: no radio is lit,
    // because none of them describes what is actually configured.
    static constexpr int kMixed = -99;

    static const char* LevelName(int value)
    {
        for (const LevelColumn& c : s_columns)
            if (c.value == value) return c.header;
        return "?";
    }

    // Centers the next radio button in the current table cell. A left-aligned
    // grid of radios is readable only by counting columns.
    static void CenterInCell()
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w     = ImGui::GetFrameHeight();
        if (avail > w)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
    }

    // -----------------------------------------------------------------------
    // ModLoader + game levels (the two persisted settings)
    // -----------------------------------------------------------------------
    static void RenderPersistedLevels()
    {
        ImGui::SeparatorText("Mod Loader");
        ImGui::TextDisabled("Written to ModLoader\\Logs\\ModLoader.log. Saved to modloader.ini.");
        ImGui::Spacing();

        static const char*    s_levelNames[]    = { "Trace", "Debug", "Info", "Warn", "Error" };
        static const wchar_t* s_levelNamesIni[] = { L"TRACE", L"DEBUG", L"INFO", L"WARN", L"ERROR" };

        int currentLevel = static_cast<int>(LogToFile::g_minLevel);
        if (ImGui::Combo("Log Level", &currentLevel, s_levelNames, 5))
        {
            LogToFile::g_minLevel = static_cast<LogToFile::Level>(currentLevel);
            const wchar_t* iniPath = UI::GlobalSettings::GetIniPath();
            if (iniPath && iniPath[0] != L'\0')
                WritePrivateProfileStringW(L"Logging", L"Level", s_levelNamesIni[currentLevel], iniPath);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Minimum level for the mod loader's own output, and the starting\n"
                              "level for every plugin that has no override below.\n\n"
                              "Takes effect immediately. Persisted to modloader.ini.");

        // Game-side UE log verbosity. Separate from the modloader's own log level
        // above -- this raises the game's UE_LOG categories so their output reaches
        // StarRupture.log.
        {
            using GameLevel = Hooks::LogVerbosity::Level;

            static const char*     s_gameLevelNames[] = {
                "Default", "Error", "Warning", "Display", "Log", "Verbose", "VeryVerbose"
            };
            static const GameLevel s_gameLevels[] = {
                GameLevel::Default, GameLevel::Error,   GameLevel::Warning, GameLevel::Display,
                GameLevel::Log,     GameLevel::Verbose, GameLevel::VeryVerbose
            };
            constexpr int kGameLevelCount = static_cast<int>(std::size(s_gameLevels));

            const GameLevel current = Hooks::LogVerbosity::GetLevel();
            int curIdx = 0;
            for (int i = 0; i < kGameLevelCount; ++i)
                if (s_gameLevels[i] == current) { curIdx = i; break; }

            const bool available = Hooks::LogVerbosity::IsInstalled();
            if (!available)
                ImGui::BeginDisabled();

            if (ImGui::Combo("Game Log Verbosity", &curIdx, s_gameLevelNames, kGameLevelCount))
                Hooks::LogVerbosity::SetLevel(s_gameLevels[curIdx]);

            if (!available)
                ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                if (available)
                    ImGui::SetTooltip(
                        "Verbosity of the GAME's own UE log categories, written to\n"
                        "StarRupture.log (not the modloader log above).\n\n"
                        "'Default' leaves the game's shipped settings alone. Any other value\n"
                        "applies to every log category at once, replacing whatever per-category\n"
                        "levels the game configured -- each category is still capped at the\n"
                        "verbosity it was compiled with, so nothing can exceed that.\n\n"
                        "Takes effect immediately and is re-applied early on the next launch.\n"
                        "Persisted to modloader.ini.");
                else
                    ImGui::SetTooltip(
                        "Unavailable: the engine log-suppression symbols could not be\n"
                        "resolved on this game build. See modloader.log for details.");
            }
        }
    }

    // -----------------------------------------------------------------------
    // Per-plugin levels
    // -----------------------------------------------------------------------
    void Render()
    {
        ImGui::Spacing();
        RenderPersistedLevels();

        ImGui::Spacing();
        ImGui::SeparatorText("Plugins");

        // GetAllPluginStatuses returns the TOTAL record count, which can exceed
        // the number it copied -- clamp before iterating.
        static PluginManager::PluginStatus statuses[64];
        const int total = PluginManager::GetAllPluginStatuses(statuses, 64);
        const int count = total < 64 ? total : 64;

        ImGui::TextDisabled("Give one plugin its own level to read its output without the rest of the");
        ImGui::TextDisabled("log drowning it -- or to silence a noisy one without turning everything down.");
        ImGui::Spacing();

        ImGui::TextColored(UI::Theme::AccentColorVec4(1.0f), "Not saved.");
        ImGui::SameLine();
        ImGui::TextDisabled("Every plugin is back on Default next time the game starts.");
        ImGui::Spacing();

        if (count == 0)
        {
            ImGui::TextDisabled("No plugins found.");
            return;
        }

        // ---- Set-all row --------------------------------------------------
        // Reflects the plugins rather than remembering the last click: if they
        // have drifted apart since (a per-plugin radio, a reload) no option is
        // lit, which is honest about there being no single answer.
        //
        // Records with no PluginInfo name are skipped throughout: one never got
        // as far as PluginInit, so it has logged nothing and has no level to
        // set. Counting it would report "mixed" for a set of plugins that in
        // fact all agree.
        int allValue = kMixed;
        bool anyNamed = false;
        for (int i = 0; i < count; ++i)
        {
            if (!statuses[i].name[0]) continue;

            const int over = PluginLogLevels::GetOverride(statuses[i].name);
            if (!anyNamed)
            {
                allValue = over;
                anyNamed = true;
            }
            else if (over != allValue)
            {
                allValue = kMixed;
                break;
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("All plugins");
        ImGui::SameLine();
        if (allValue == kMixed)
            ImGui::TextDisabled("(mixed)");
        else
            ImGui::TextDisabled("(%s)", LevelName(allValue));

        ImGui::SameLine(0.0f, 20.0f);
        for (int c = 0; c < kColumnCount; ++c)
        {
            if (c > 0) ImGui::SameLine();

            char id[64];
            snprintf(id, sizeof(id), "%s##all_%d", s_columns[c].header, c);
            if (ImGui::RadioButton(id, allValue == s_columns[c].value))
            {
                for (int i = 0; i < count; ++i)
                    PluginLogLevels::SetOverride(statuses[i].name, s_columns[c].value);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Set every plugin to %s.\n\n%s",
                                  s_columns[c].header, s_columns[c].tooltip);
        }

        ImGui::Spacing();

        // ---- Per-plugin grid ----------------------------------------------
        // No ScrollY: the tab's own content child already scrolls, and a table
        // with its own scroll region would eat the rest of the tab's height and
        // push the reset row below it off screen.
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit;

        // Column width sized off the header text plus the radio itself, so the
        // grid holds its shape at any font scale.
        const float radioW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;

        if (ImGui::BeginTable("##plugin_log_levels", 1 + kColumnCount, tableFlags))
        {
            ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
            for (int c = 0; c < kColumnCount; ++c)
            {
                const float w = ImGui::CalcTextSize(s_columns[c].header).x + radioW;
                ImGui::TableSetupColumn(s_columns[c].header, ImGuiTableColumnFlags_WidthFixed, w);
            }
            ImGui::TableHeadersRow();

            for (int i = 0; i < count; ++i)
            {
                const PluginManager::PluginStatus& s = statuses[i];

                // A record with no PluginInfo name never reached PluginInit, so
                // it has logged nothing and has nothing to configure. Its DLL
                // name is shown so the row is not silently missing.
                const bool hasName = s.name[0] != '\0';

                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                if (!hasName)
                {
                    ImGui::TextDisabled("%s", s.fileName[0] ? s.fileName : "?");
                }
                else if (s.isLoaded)
                {
                    ImGui::TextUnformatted(s.name);
                }
                else
                {
                    ImGui::TextDisabled("%s", s.name);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(unloaded)");
                }

                const int current = hasName ? PluginLogLevels::GetOverride(s.name)
                                            : PluginLogLevels::kInherit;

                for (int c = 0; c < kColumnCount; ++c)
                {
                    ImGui::TableSetColumnIndex(1 + c);
                    CenterInCell();

                    char id[32];
                    snprintf(id, sizeof(id), "##lvl_%d", c);

                    if (!hasName) ImGui::BeginDisabled();
                    if (ImGui::RadioButton(id, current == s_columns[c].value))
                        PluginLogLevels::SetOverride(s.name, s_columns[c].value);
                    if (!hasName) ImGui::EndDisabled();

                    if (hasName && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        ImGui::SetTooltip("%s -- %s\n\n%s",
                                          s.name, s_columns[c].header, s_columns[c].tooltip);
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();

        // Sampled once: the button's own click clears the overrides, so
        // re-reading it after would leave the BeginDisabled/EndDisabled pair
        // unbalanced for the rest of the frame.
        const bool anyOverrides = PluginLogLevels::AnyOverrides();

        if (!anyOverrides) ImGui::BeginDisabled();
        if (ImGui::Button("Reset All To Default"))
            PluginLogLevels::ClearAll();
        if (!anyOverrides) ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("%s", anyOverrides
            ? "Puts every plugin back on the ModLoader Log Level."
            : "Every plugin is already on the ModLoader Log Level.");
    }
}

#endif // MODLOADER_CLIENT_BUILD
