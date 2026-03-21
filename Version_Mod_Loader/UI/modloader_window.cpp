#include "pch.h"
#include "modloader_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "plugin_panel_registry.h"
#include "plugins/plugin_manager.h"
#include "config/config_manager.h"
#include "global_settings.h"
#include "logging/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

// Build tag is set by CI; fall back to a local placeholder.
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "local-build"
#endif

namespace UI::ModLoaderWindow
{
    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    static bool s_isOpen = false;
    static int  s_selectedPlugin = -1;  // index in Plugins tab

    // Config tab: per-key editable buffers.
    // We parse the INI once when the selection changes, then cache.
    struct ConfigKV
    {
        char section[64];
        char key[64];
        char value[256];
    };
    static std::vector<ConfigKV> s_configEntries;
    static int  s_lastConfigPlugin = -1;  // plugin index for cached entries

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    // Build the absolute path to <pluginName>.ini under the config directory.
    static bool GetPluginIniPath(const char* pluginName, wchar_t* outPath, size_t outLen)
    {
        const wchar_t* configDir = ModLoaderLogger::GetConfigDirectory();
        if (!configDir || !pluginName) return false;
        swprintf_s(outPath, outLen, L"%s\\%S.ini", configDir, pluginName);
        return true;
    }

    // Parse all sections + keys from the plugin's INI file into s_configEntries.
    static void LoadConfigEntries(const char* pluginName)
    {
        s_configEntries.clear();

        wchar_t iniPath[MAX_PATH];
        if (!GetPluginIniPath(pluginName, iniPath, MAX_PATH))
            return;

        // Enumerate section names (double-NUL terminated list)
        wchar_t sectionBuf[4096] = {};
        GetPrivateProfileSectionNamesW(sectionBuf, ARRAYSIZE(sectionBuf), iniPath);

        for (const wchar_t* sec = sectionBuf; *sec; sec += wcslen(sec) + 1)
        {
            // Read all key=value pairs in this section
            wchar_t kvBuf[8192] = {};
            GetPrivateProfileSectionW(sec, kvBuf, ARRAYSIZE(kvBuf), iniPath);

            for (const wchar_t* kv = kvBuf; *kv; kv += wcslen(kv) + 1)
            {
                // Find '='
                const wchar_t* eq = wcschr(kv, L'=');
                if (!eq) continue;

                ConfigKV entry = {};
                // Section (narrow)
                snprintf(entry.section, sizeof(entry.section), "%ls", sec);
                // Key (narrow, up to '=')
                int keyLen = static_cast<int>(eq - kv);
                if (keyLen <= 0 || keyLen >= static_cast<int>(sizeof(entry.key))) continue;
                snprintf(entry.key, sizeof(entry.key), "%.*ls", keyLen, kv);
                // Value (narrow, after '=')
                snprintf(entry.value, sizeof(entry.value), "%ls", eq + 1);

                s_configEntries.push_back(entry);
            }
        }
    }

    // Write a changed value back to disk and fire config-change notifications.
    static void CommitConfigChange(const char* pluginName, ConfigKV& kv)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetPluginIniPath(pluginName, iniPath, MAX_PATH)) return;

        wchar_t wsec[64], wkey[64], wval[256];
        swprintf_s(wsec, L"%S", kv.section);
        swprintf_s(wkey, L"%S", kv.key);
        swprintf_s(wval, L"%S", kv.value);
        WritePrivateProfileStringW(wsec, wkey, wval, iniPath);

        UI::PluginPanelRegistry::FireConfigChanged(kv.section, kv.key, kv.value);
    }

    // -----------------------------------------------------------------------
    // Tab renderers
    // -----------------------------------------------------------------------

    static void RenderPluginsTab()
    {
        static const PluginInfo* infos[64];
        int count = ModLoaderLogger::GetLoadedPluginInfos(infos, 64);

        if (count == 0)
        {
            ImGui::TextDisabled("No plugins loaded.");
            return;
        }

        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##plugins", 4, tableFlags))
        {
            ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Version",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Author",      ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status",      ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < count; ++i)
            {
                const PluginInfo* info = infos[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(info->name    ? info->name    : "?");
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(info->version ? info->version : "?");
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(info->author  ? info->author  : "?");
                ImGui::TableSetColumnIndex(3); ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Active");
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Hot reload / stop / start will be available in a future update.");
    }

    // Returns the ConfigEntry for (section, key) from schema, or nullptr.
    static const ConfigEntry* FindSchemaEntry(const ConfigSchema* schema,
                                              const char* section, const char* key)
    {
        if (!schema) return nullptr;
        for (int i = 0; i < schema->entryCount; ++i)
        {
            const ConfigEntry& e = schema->entries[i];
            if (strcmp(e.section, section) == 0 && strcmp(e.key, key) == 0)
                return &e;
        }
        return nullptr;
    }

    // Render a single config row using type information from schema entry (may be null).
    static void RenderConfigEntry(ConfigKV& kv, const ConfigEntry* e, const char* pluginName)
    {
        char id[128];
        snprintf(id, sizeof(id), "##%s_%s", kv.section, kv.key);

        // Show reset button for all entries except General.Enabled
        bool showReset = e && e->defaultValue && e->defaultValue[0] &&
                         !(strcmp(kv.section, "General") == 0 && strcmp(kv.key, "Enabled") == 0);

        // Width to reserve for the reset button: frame height (square button) + spacing
        float resetBtnW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        float inputWidth = showReset ? -resetBtnW : -1.0f;

        bool widgetHovered = false;

        if (e && e->type == ConfigValueType::Boolean)
        {
            bool bval = (strcmp(kv.value, "true") == 0 || strcmp(kv.value, "1") == 0);
            char label[128];
            snprintf(label, sizeof(label), "%s%s", kv.key, id);
            if (ImGui::Checkbox(label, &bval))
            {
                strncpy_s(kv.value, bval ? "true" : "false", _TRUNCATE);
                CommitConfigChange(pluginName, kv);
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else if (e && e->type == ConfigValueType::Integer)
        {
            int ival = atoi(kv.value);
            bool hasRange = e->rangeMax > e->rangeMin;
            ImGui::TextUnformatted(kv.key);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(inputWidth);
            if (hasRange)
            {
                if (ImGui::SliderInt(id, &ival, (int)e->rangeMin, (int)e->rangeMax))
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                if (ImGui::IsItemDeactivated())
                    CommitConfigChange(pluginName, kv);
            }
            else
            {
                if (ImGui::InputInt(id, &ival, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                    CommitConfigChange(pluginName, kv);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                    CommitConfigChange(pluginName, kv);
                }
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else if (e && e->type == ConfigValueType::Float)
        {
            float fval = strtof(kv.value, nullptr);
            bool hasRange = e->rangeMax > e->rangeMin;
            ImGui::TextUnformatted(kv.key);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(inputWidth);
            if (hasRange)
            {
                if (ImGui::SliderFloat(id, &fval, e->rangeMin, e->rangeMax, "%.6g"))
                    snprintf(kv.value, sizeof(kv.value), "%.6g", fval);
                if (ImGui::IsItemDeactivated())
                    CommitConfigChange(pluginName, kv);
            }
            else
            {
                if (ImGui::InputFloat(id, &fval, 0.0f, 0.0f, "%.6g",
                                      ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    snprintf(kv.value, sizeof(kv.value), "%.6g", fval);
                    CommitConfigChange(pluginName, kv);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    snprintf(kv.value, sizeof(kv.value), "%.6g", fval);
                    CommitConfigChange(pluginName, kv);
                }
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else
        {
            // String or no schema entry: raw text input (original behaviour)
            ImGui::TextUnformatted(kv.key);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(inputWidth);
            if (ImGui::InputText(id, kv.value, sizeof(kv.value),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                CommitConfigChange(pluginName, kv);
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitConfigChange(pluginName, kv);
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }

        // Description tooltip (captured before the reset button changes IsItemHovered)
        if (e && e->description && e->description[0] && widgetHovered)
            ImGui::SetTooltip("%s", e->description);

        // Reset button
        if (showReset)
        {
            char resetId[160];
            snprintf(resetId, sizeof(resetId), "R##r_%s_%s", kv.section, kv.key);
            ImGui::SameLine();
            if (ImGui::SmallButton(resetId))
            {
                strncpy_s(kv.value, e->defaultValue, _TRUNCATE);
                CommitConfigChange(pluginName, kv);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Reset to default: %s", e->defaultValue);
        }
    }

    static void RenderConfigTab(IModLoaderImGui* imgui)
    {
        static const PluginInfo* infos[64];
        int count = ModLoaderLogger::GetLoadedPluginInfos(infos, 64);

        if (count == 0)
        {
            ImGui::TextDisabled("No plugins loaded.");
            return;
        }

        // Left panel: plugin list
        const float LIST_WIDTH = 160.0f;
        ImGui::BeginChild("##cfg_list", ImVec2(LIST_WIDTH, 0), true);
        for (int i = 0; i < count; ++i)
        {
            const char* name = infos[i]->name ? infos[i]->name : "?";
            bool selected = (s_selectedPlugin == i);
            if (ImGui::Selectable(name, selected))
                s_selectedPlugin = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel: config editor
        ImGui::BeginChild("##cfg_editor", ImVec2(0, 0), false);

        if (s_selectedPlugin < 0 || s_selectedPlugin >= count)
        {
            ImGui::TextDisabled("Select a plugin on the left.");
        }
        else
        {
            const PluginInfo* info = infos[s_selectedPlugin];

            // Reload cache when selection changes
            if (s_lastConfigPlugin != s_selectedPlugin)
            {
                s_lastConfigPlugin = s_selectedPlugin;
                LoadConfigEntries(info->name);
            }

            if (s_configEntries.empty())
            {
                ImGui::TextDisabled("No config file found for this plugin.");
            }
            else
            {
                const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(info->name);

                ImGui::TextDisabled("Hover a setting for its description.  Changes are saved immediately.");
                ImGui::Spacing();

                const char* curSection = nullptr;
                for (auto& kv : s_configEntries)
                {
                    // Section header
                    if (!curSection || strcmp(curSection, kv.section) != 0)
                    {
                        if (curSection) ImGui::Spacing();
                        ImGui::SeparatorText(kv.section);
                        curSection = kv.section;
                    }

                    const ConfigEntry* entry = FindSchemaEntry(schema, kv.section, kv.key);
                    RenderConfigEntry(kv, entry, info->name);
                }

                // Plugin panels button row
                ImGui::Spacing();
                ImGui::SeparatorText("Plugin Tools");
                UI::PluginPanelRegistry::RenderPanelButtons(imgui);
            }
        }

        ImGui::EndChild();
    }

    static void RenderGlobalSettingsTab()
    {
        ImGui::Spacing();
        ImGui::SeparatorText("HUD Overlays");
        ImGui::TextDisabled("These are prefixed to the ModLoader overlay in the bottom corner of the screen.");
        ImGui::Spacing();

        bool showFPS = UI::GlobalSettings::GetShowFPS();
        if (ImGui::Checkbox("Show FPS", &showFPS))
            UI::GlobalSettings::SetShowFPS(showFPS);

        bool showWorld = UI::GlobalSettings::GetShowWorldName();
        if (ImGui::Checkbox("Show Current World Name", &showWorld))
            UI::GlobalSettings::SetShowWorldName(showWorld);

        bool showPos = UI::GlobalSettings::GetShowPlayerPosition();
        if (ImGui::Checkbox("Show Player Position", &showPos))
            UI::GlobalSettings::SetShowPlayerPosition(showPos);

        ImGui::Spacing();
        ImGui::TextDisabled("Settings are saved to modloader.ini immediately.");
    }

    static void RenderAboutTab()
    {
        ImGui::Spacing();
        ImGui::TextUnformatted("StarRupture ModLoader By AlienX");
        ImGui::Spacing();
        ImGui::TextDisabled("Build: " MODLOADER_BUILD_TAG);

        char imguiVer[64];
        snprintf(imguiVer, sizeof(imguiVer), "Dear ImGui %s", ImGui::GetVersion());
        ImGui::TextDisabled("%s", imguiVer);

        ImGui::Spacing();
        ImGui::SeparatorText("Loaded Plugins");

        static const PluginInfo* infos[64];
        int count = ModLoaderLogger::GetLoadedPluginInfos(infos, 64);
        for (int i = 0; i < count; ++i)
        {
            const PluginInfo* info = infos[i];
            char line[256];
            snprintf(line, sizeof(line), "%s v%s by %s",
                info->name    ? info->name    : "?",
                info->version ? info->version : "?",
                info->author  ? info->author  : "?");
            ImGui::TextUnformatted(line);
            if (info->description && info->description[0])
            {
                ImGui::TextDisabled("  %s", info->description);
            }
        }
        if (count == 0)
            ImGui::TextDisabled("(none)");
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    void Toggle()
    {
        s_isOpen = !s_isOpen;
    }

    bool IsOpen()
    {
        return s_isOpen;
    }

    void Render(IModLoaderImGui* imgui)
    {
        if (!s_isOpen)
            return;

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_FirstUseEver,
            ImVec2(0.5f, 0.5f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_None;
        if (!ImGui::Begin("ModLoader##main", &s_isOpen, flags))
        {
            ImGui::End();
            return;
        }

        if (ImGui::BeginTabBar("##tabs"))
        {
            if (ImGui::BeginTabItem("Plugins"))
            {
                RenderPluginsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Config"))
            {
                RenderConfigTab(imgui);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Global Settings"))
            {
                RenderGlobalSettingsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About"))
            {
                RenderAboutTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }
}

#endif // MODLOADER_CLIENT_BUILD
