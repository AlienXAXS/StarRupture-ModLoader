#include "pch.h"
#include "modloader_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "plugin_panel_registry.h"
#include "plugins/plugin_manager.h"
#include "config/config_manager.h"
#include "global_settings.h"
#include "theme.h"
#include "logging/log.h"
#include "hooks/input/keybind_registry.h"
#include <cmath>
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
    static int  s_activeTab = 0;        // index into the icon tab strip (Plugins/Config/Settings/About)

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

    struct RebindState
    {
        bool active           = false;
        bool pendingOpen      = false; // set inside a table; OpenPopup deferred to modal site
        char section[64]      = {};
        char cfgKey[64]       = {};   // the INI key name (not keyboard key)
        char pluginName[64]   = {};
        bool waitingForRelease = false;
    };
    static RebindState s_rebind;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    // Forward declaration (defined after RenderPluginsTab).
    static const ConfigEntry* FindSchemaEntry(const ConfigSchema* schema,
                                              const char* section, const char* key);

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

        // For each Keybind entry in the schema, read and apply the companion
        // <key>Blocking flag so the runtime blocking state is always current
        // when the config panel is opened.
        const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(pluginName);
        if (schema)
        {
            for (auto& kv : s_configEntries)
            {
                const ConfigEntry* schEntry = FindSchemaEntry(schema, kv.section, kv.key);
                if (!schEntry || schEntry->type != ConfigValueType::Keybind) continue;
                if (!kv.value[0]) continue;

                wchar_t wsec[64], wblkKey[128];
                swprintf_s(wsec, L"%S", kv.section);
                swprintf_s(wblkKey, L"%SBlocking", kv.key);
                bool blocking = (GetPrivateProfileIntW(wsec, wblkKey, 0, iniPath) != 0);
                Hooks::Input::SetComboBlocking(kv.value, blocking);
            }
        }
    }

    // Write a changed value back to disk and fire config-change notifications.
    static void CommitConfigChange(const char* pluginName, ConfigKV& kv,
                                   const char* oldValue = nullptr,
                                   const ConfigEntry* entry = nullptr)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetPluginIniPath(pluginName, iniPath, MAX_PATH)) return;

        wchar_t wsec[64], wkey[64], wval[256];
        swprintf_s(wsec, L"%S", kv.section);
        swprintf_s(wkey, L"%S", kv.key);
        swprintf_s(wval, L"%S", kv.value);
        WritePrivateProfileStringW(wsec, wkey, wval, iniPath);

        // If this is a Keybind entry and the value actually changed, live-rebind
        // any active keybind registrations for this plugin and transfer blocking state.
        if (entry && entry->type == ConfigValueType::Keybind &&
            oldValue && strcmp(oldValue, kv.value) != 0)
        {
            // Transfer blocking state from the old combo to the new one.
            // Read from INI (the ground truth) rather than the runtime map so this
            // works correctly even if the map entry was never explicitly set.
            {
                wchar_t blkSec[64], wblkKey[128];
                swprintf_s(blkSec, L"%S", kv.section);
                swprintf_s(wblkKey, L"%SBlocking", kv.key);
                bool wasBlocking = (GetPrivateProfileIntW(blkSec, wblkKey, 0, iniPath) != 0);
                Hooks::Input::SetComboBlocking(kv.value, wasBlocking);
                // Remove any stale entry for the old combo so it does not linger.
                Hooks::Input::SetComboBlocking(oldValue, false);
            }

            Hooks::Input::UpdateKeybindByName(pluginName, oldValue, kv.value);
        }

        UI::PluginPanelRegistry::FireConfigChanged(kv.section, kv.key, kv.value);
    }

    // -----------------------------------------------------------------------
    // Tab renderers
    // -----------------------------------------------------------------------

    static void RenderPluginsTab()
    {
        static PluginManager::PluginStatus statuses[64];
        int count = PluginManager::GetAllPluginStatuses(statuses, 64);

        if (count == 0)
        {
            ImGui::TextDisabled("No plugins found.");
            return;
        }

        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##plugins", 5, tableFlags))
        {
            float fs = ImGui::GetFontSize();
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed,  fs * 5.4f);
            ImGui::TableSetupColumn("Author",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status",  ImGuiTableColumnFlags_WidthFixed,  fs * 6.9f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,  fs * 16.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < count; ++i)
            {
                const PluginManager::PluginStatus& s = statuses[i];
                ImGui::TableNextRow();
                if (s.isLoaded)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(UI::Theme::AccentColorVec4(0.10f)));

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(s.name[0] ? s.name : "?");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(s.version[0] ? s.version : "?");

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(s.author[0] ? s.author : "?");

                ImGui::TableSetColumnIndex(3);
                if (s.isWrongTarget)
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Cannot Load");
                else if (s.isOutOfDate)
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "Needs Update");
                else if (s.isLoaded)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Loaded");
                else
                    ImGui::TextDisabled("Unloaded");

                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(i);

                if (s.isWrongTarget)
                {
                    ImGui::TextDisabled("You're either trying to load a Server plugin on the Client, or vice versa");
                }
                else if (s.isOutOfDate)
                {
                    ImGui::TextDisabled(s.needsModLoaderUpdate
                        ? "Please Update Mod Loader Version"
                        : "Please Update The Plugin");
                }
                else
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));

                    // Unload — active only when loaded
                    if (!s.isLoaded) ImGui::BeginDisabled();
                    if (ImGui::Button("UNLOAD"))
                        PluginManager::UnloadPlugin(i);
                    if (!s.isLoaded) ImGui::EndDisabled();

                    ImGui::SameLine();

                    // Load — active only when unloaded
                    if (s.isLoaded) ImGui::BeginDisabled();
                    if (ImGui::Button("LOAD"))
                        PluginManager::ReloadPlugin(i);
                    if (s.isLoaded) ImGui::EndDisabled();

                    ImGui::SameLine();

                    // Reload — always active; accented as the primary action.
                    ImGui::PushStyleColor(ImGuiCol_Button, UI::Theme::AccentColorVec4(0.20f));
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::AccentColorVec4(1.0f));
                    if (ImGui::Button("RELOAD"))
                        PluginManager::ReloadPlugin(i);
                    ImGui::PopStyleColor(2);

                    ImGui::PopStyleVar();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
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

    static void FormatFloat(char* buf, size_t sz, float v)
    {
        snprintf(buf, sz, "%.6f", v);
        char* dot = strchr(buf, '.');
        if (dot)
        {
            char* end = buf + strlen(buf) - 1;
            while (end > dot && *end == '0') *end-- = '\0';
            if (end == dot) *end = '\0';
        }
    }

    // Render one config row inside an already-open 3-column table:
    //   Col 0 (Label)   -- setting name, description tooltip on hover
    //   Col 1 (Widget)  -- the editable control, fills available width
    //   Col 2 (Actions) -- blocking checkbox (keybind only) + reset button
    static void RenderConfigEntry(ConfigKV& kv, const ConfigEntry* e, const char* pluginName)
    {
        ImGui::TableNextRow();

        char id[128];
        snprintf(id, sizeof(id), "##%s_%s", kv.section, kv.key);

        bool showReset = e && e->defaultValue && e->defaultValue[0] &&
                         !(strcmp(kv.section, "General") == 0 && strcmp(kv.key, "Enabled") == 0);

        // ---- Col 0: label ------------------------------------------------
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kv.key);
        if (e && e->description && e->description[0] &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", e->description);

        // ---- Col 1: widget -----------------------------------------------
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN); // fill the column

        bool widgetHovered = false;

        if (e && e->type == ConfigValueType::Boolean)
        {
            bool bval = (strcmp(kv.value, "true") == 0 || strcmp(kv.value, "1") == 0);
            char lbl[128];
            snprintf(lbl, sizeof(lbl), "##chk%s", id);
            if (UI::Theme::ToggleSwitch(lbl, &bval))
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
            if (hasRange)
            {
                if (ImGui::SliderInt(id, &ival, (int)e->rangeMin, (int)e->rangeMax))
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                if (ImGui::IsItemDeactivated())
                    CommitConfigChange(pluginName, kv);
            }
            else
            {
                if (ImGui::InputInt(id, &ival, 1, 10))
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
            if (hasRange)
            {
                if (ImGui::SliderFloat(id, &fval, e->rangeMin, e->rangeMax, "%.6f"))
                    FormatFloat(kv.value, sizeof(kv.value), fval);
                if (ImGui::IsItemDeactivated())
                    CommitConfigChange(pluginName, kv);
            }
            else
            {
                if (ImGui::InputFloat(id, &fval, 0.0f, 0.0f, "%.6f"))
                {
                    FormatFloat(kv.value, sizeof(kv.value), fval);
                    CommitConfigChange(pluginName, kv);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    FormatFloat(kv.value, sizeof(kv.value), fval);
                    CommitConfigChange(pluginName, kv);
                }
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else if (e && e->type == ConfigValueType::Keybind)
        {
            // Current bind label + Rebind button, side by side, left-aligned.
            const char* bindLabel = (kv.value[0] != '\0') ? kv.value : "(none)";
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", bindLabel);
            ImGui::SameLine();
            char rebindId[160];
            snprintf(rebindId, sizeof(rebindId), "Rebind##rb_%s_%s", kv.section, kv.key);
            if (ImGui::SmallButton(rebindId))
            {
                strncpy_s(s_rebind.section,    kv.section, _TRUNCATE);
                strncpy_s(s_rebind.cfgKey,     kv.key,     _TRUNCATE);
                strncpy_s(s_rebind.pluginName, pluginName, _TRUNCATE);
                s_rebind.waitingForRelease = true;
                s_rebind.active            = true;
                s_rebind.pendingOpen       = true; // OpenPopup deferred — called from outside the table
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else
        {
            // String or unknown schema entry: plain text input.
            if (ImGui::InputText(id, kv.value, sizeof(kv.value),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                CommitConfigChange(pluginName, kv);
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitConfigChange(pluginName, kv);
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }

        if (e && e->description && e->description[0] && widgetHovered)
            ImGui::SetTooltip("%s", e->description);

        // ---- Col 2: actions ----------------------------------------------
        ImGui::TableSetColumnIndex(2);

        // Blocking checkbox (keybind rows only).
        if (e && e->type == ConfigValueType::Keybind)
        {
            wchar_t iniPath[MAX_PATH];
            bool bBlocking = false;
            if (GetPluginIniPath(pluginName, iniPath, MAX_PATH))
            {
                wchar_t wsec[64], wblkKey[128];
                swprintf_s(wsec,    L"%S",        kv.section);
                swprintf_s(wblkKey, L"%SBlocking", kv.key);
                bBlocking = (GetPrivateProfileIntW(wsec, wblkKey, 0, iniPath) != 0);
            }
            char chkId[160];
            snprintf(chkId, sizeof(chkId), "##blk_%s_%s", kv.section, kv.key);
            if (UI::Theme::ToggleSwitch(chkId, &bBlocking))
            {
                wchar_t iniPath2[MAX_PATH];
                if (GetPluginIniPath(pluginName, iniPath2, MAX_PATH))
                {
                    wchar_t wsec2[64], wblkKey2[128];
                    swprintf_s(wsec2,    L"%S",        kv.section);
                    swprintf_s(wblkKey2, L"%SBlocking", kv.key);
                    WritePrivateProfileStringW(wsec2, wblkKey2, bBlocking ? L"1" : L"0", iniPath2);
                }
                Hooks::Input::SetComboBlocking(kv.value, bBlocking);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Block: when ticked, this combo is consumed by the\n"
                                  "plugin -- the game will not also react to it.\n"
                                  "Enable if the key conflicts with a game action.");
            ImGui::SameLine();
        }

        // Reset button.
        if (showReset)
        {
            char resetId[160];
            snprintf(resetId, sizeof(resetId), "R##r_%s_%s", kv.section, kv.key);
            if (ImGui::SmallButton(resetId))
            {
                strncpy_s(kv.value, e->defaultValue, _TRUNCATE);
                CommitConfigChange(pluginName, kv);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Reset to default: %s", e->defaultValue);
        }
    }

    // Returns true if the given VK is a modifier key (Ctrl/Shift/Alt left or right).
    static bool IsModifierVK(int vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
               vk == VK_LMENU    || vk == VK_RMENU;
    }

    static void RenderRebindModal()
    {
        if (!s_rebind.active)
            return;

        // OpenPopup must be called at the same ID-stack level as BeginPopupModal.
        // The button that sets pendingOpen lives inside a BeginTable, so we defer here.
        if (s_rebind.pendingOpen)
        {
            ImGui::OpenPopup("##rebind_modal");
            s_rebind.pendingOpen = false;
        }

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(340, 110), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("##rebind_modal", nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::TextUnformatted("Press a key (with optional Ctrl/Shift/Alt), or ESC to cancel.");
            ImGui::Spacing();
            ImGui::TextDisabled("Binding: %s / %s", s_rebind.section, s_rebind.cfgKey);
            ImGui::Spacing();

            // Phase 1: wait for all keys to be released so the button click doesn't register.
            if (s_rebind.waitingForRelease)
            {
                bool anyDown = false;
                for (int vk = 0x01; vk <= 0xFE; ++vk)
                {
                    if (GetAsyncKeyState(vk) & 0x8000) { anyDown = true; break; }
                }
                if (!anyDown)
                    s_rebind.waitingForRelease = false;

                ImGui::TextDisabled("(release keys...)");
                ImGui::EndPopup();
                return;
            }

            // Phase 2: ESC cancels.
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                s_rebind.active = false;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }

            // Show live modifier state as feedback while waiting for the base key.
            EModKeyModifiers curMods = Hooks::Input::SampleCurrentModifiers();
            {
                char preview[64] = {};
                if (curMods & EModKeyMod_Ctrl)  { if (preview[0]) strncat_s(preview, " + ", _TRUNCATE); strncat_s(preview, "Ctrl",  _TRUNCATE); }
                if (curMods & EModKeyMod_Shift) { if (preview[0]) strncat_s(preview, " + ", _TRUNCATE); strncat_s(preview, "Shift", _TRUNCATE); }
                if (curMods & EModKeyMod_Alt)   { if (preview[0]) strncat_s(preview, " + ", _TRUNCATE); strncat_s(preview, "Alt",   _TRUNCATE); }
                if (preview[0])
                    strncat_s(preview, " + ...", _TRUNCATE);
                else
                    strncpy_s(preview, "...", _TRUNCATE);
                ImGui::Text("%s", preview);
            }

            // Phase 3: scan for a non-modifier key press.
            for (int vk = 0x01; vk <= 0xFE; ++vk)
            {
                if (vk == VK_ESCAPE)      continue;
                if (IsModifierVK(vk))     continue;
                if (!(GetAsyncKeyState(vk) & 0x8000)) continue;

                EModKey mk = Hooks::Input::VKToModKey(vk);
                if (mk == EModKey::Unknown) continue;

                // Build the combo string and write it to the config entry.
                char comboStr[64];
                Hooks::Input::FormatComboString(mk, curMods, comboStr, sizeof(comboStr));

                for (auto& kv : s_configEntries)
                {
                    if (strcmp(kv.section, s_rebind.section) == 0 &&
                        strcmp(kv.key, s_rebind.cfgKey) == 0)
                    {
                        char oldValue[256];
                        strncpy_s(oldValue, kv.value, _TRUNCATE);
                        strncpy_s(kv.value, comboStr, _TRUNCATE);

                        // Find the schema entry so CommitConfigChange can trigger live-rebind.
                        const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(s_rebind.pluginName);
                        const ConfigEntry* schEntry = FindSchemaEntry(schema, kv.section, kv.key);
                        CommitConfigChange(s_rebind.pluginName, kv, oldValue, schEntry);
                        break;
                    }
                }

                s_rebind.active = false;
                ImGui::CloseCurrentPopup();
                break;
            }

            ImGui::EndPopup();
        }
        else
        {
            // Popup was closed externally.
            s_rebind.active = false;
        }
    }

    static void RenderConfigTab(IModLoaderImGui* imgui)
    {
        static const PluginInfo* infos[64];
        int count = PluginManager::GetLoadedPluginInfos(infos, 64);

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

            // Plugin panels button row (shown before config entries)
            ImGui::SeparatorText("Plugin Tools");
            UI::PluginPanelRegistry::RenderPanelButtons(imgui, info->name);
            ImGui::Spacing();

            if (s_configEntries.empty())
            {
                ImGui::TextDisabled("No config file found for this plugin.");
            }
            else
            {
                const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(info->name);

                ImGui::TextDisabled("Hover a setting for its description.  Changes are saved immediately.");
                ImGui::Spacing();

                // Actions column width: blocking checkbox + spacing + reset button.
                // Sized to fit the widest possible content (keybind rows).
                const float fh       = ImGui::GetFrameHeight();
                const float spacing  = ImGui::GetStyle().ItemSpacing.x;
                const float actionsW = fh + spacing + fh * 0.75f; // checkbox + R button

                // One 3-column table per section so separators span full width
                // and all rows within a section share the same column edges.
                //   Col 0  Label   -- fixed 160px
                //   Col 1  Widget  -- stretches to fill remaining space
                //   Col 2  Actions -- fixed (blocking checkbox + reset)
                const ImGuiTableFlags tblFlags =
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_PadOuterX;

                const char* curSection = nullptr;
                bool        tableOpen  = false;

                for (auto& kv : s_configEntries)
                {
                    if (!curSection || strcmp(curSection, kv.section) != 0)
                    {
                        if (tableOpen) { ImGui::EndTable(); tableOpen = false; }
                        if (curSection) ImGui::Spacing();

                        ImGui::SeparatorText(kv.section);
                        curSection = kv.section;

                        char tblId[128];
                        snprintf(tblId, sizeof(tblId), "##cfg_%s", kv.section);
                        if (ImGui::BeginTable(tblId, 3, tblFlags))
                        {
                            ImGui::TableSetupColumn("##lbl",    ImGuiTableColumnFlags_WidthFixed,   160.0f);
                            ImGui::TableSetupColumn("##widget", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("##acts",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, actionsW);
                            tableOpen = true;
                        }
                    }

                    if (tableOpen)
                    {
                        const ConfigEntry* entry = FindSchemaEntry(schema, kv.section, kv.key);
                        RenderConfigEntry(kv, entry, info->name);
                    }
                }

                if (tableOpen) ImGui::EndTable();
            }
        }

        RenderRebindModal();

        ImGui::EndChild();
    }

    static void RenderGlobalSettingsTab()
    {
        ImGui::Spacing();
        ImGui::SeparatorText("HUD Overlays");
        ImGui::TextDisabled("These are prefixed to the ModLoader overlay in the bottom corner of the screen.");
        ImGui::Spacing();

        bool showFPS = UI::GlobalSettings::GetShowFPS();
        if (UI::Theme::ToggleSwitch("Show FPS", &showFPS))
            UI::GlobalSettings::SetShowFPS(showFPS);

        bool showWorld = UI::GlobalSettings::GetShowWorldName();
        if (UI::Theme::ToggleSwitch("Show Current World Name", &showWorld))
            UI::GlobalSettings::SetShowWorldName(showWorld);

        bool showPos = UI::GlobalSettings::GetShowPlayerPosition();
        if (UI::Theme::ToggleSwitch("Show Player Position", &showPos))
            UI::GlobalSettings::SetShowPlayerPosition(showPos);

        ImGui::Spacing();
        ImGui::SeparatorText("Logging");
        ImGui::Spacing();

        static const char*    s_levelNames[]   = { "Trace", "Debug", "Info", "Warn", "Error" };
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
            ImGui::SetTooltip("Takes effect immediately. Persisted to modloader.ini.");

        ImGui::Spacing();
        ImGui::TextDisabled("Settings are saved to modloader.ini immediately.");
    }

    // Long rectangular swatch -- click anywhere on the bar to open the
    // color picker popup (hue wheel + saturation triangle). No inline
    // RGB/hex sliders, unlike a plain ImGui::ColorEdit4.
    static void ColorBar(const char* label, ImVec4* color, float width = -1.0f)
    {
        ImGui::PushID(label);

        if (width < 0.0f)
            width = ImGui::CalcItemWidth();
        const float height = ImGui::GetFrameHeight();

        if (ImGui::ColorButton("##swatch", *color,
                ImGuiColorEditFlags_NoTooltip, ImVec2(width, height)))
            ImGui::OpenPopup("##picker");

        if (ImGui::BeginPopup("##picker"))
        {
            ImGui::ColorPicker4("##picker_widget", reinterpret_cast<float*>(color),
                ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview);
            ImGui::EndPopup();
        }

        const char* hash = strstr(label, "##");
        if (hash != label)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(label, hash);
        }

        ImGui::PopID();
    }

    static void RenderThemeTab()
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Font");
        ImGui::Spacing();

        static const char*  s_sizeLabels[] = { "Small", "Normal", "Large", "Extra Large" };
        static const float  s_sizeScales[] = { 0.75f,   1.00f,   1.25f,   1.50f };
        static const int    s_sizeCount    = 4;

        float curScale = UI::GlobalSettings::GetFontScale();
        int   curIdx   = 1;
        for (int i = 0; i < s_sizeCount; ++i)
            if (fabsf(s_sizeScales[i] - curScale) < 0.01f) { curIdx = i; break; }

        if (ImGui::Combo("Text Size", &curIdx, s_sizeLabels, s_sizeCount))
            UI::GlobalSettings::SetFontScale(s_sizeScales[curIdx]);

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scales all ImGui window text. Takes effect immediately.");

        ImGui::Spacing();

        // Font family combo -- only show entries whose font file exists on disk.
        struct FontOption { const char* iniKey; const char* displayName; const wchar_t* primaryPath; };
        static const FontOption s_fontOptions[] =
        {
            { "Default",  "Default (built-in)",   nullptr                              },
            { "Arial",    "Arial",                 L"C:\\Windows\\Fonts\\arial.ttf"    },
            { "YaHei",    "Microsoft YaHei",       L"C:\\Windows\\Fonts\\msyh.ttc"     },
            { "Meiryo",   "Meiryo",                L"C:\\Windows\\Fonts\\meiryo.ttc"   },
            { "Malgun",   "Malgun Gothic",         L"C:\\Windows\\Fonts\\malgun.ttf"   },
            { "ArialCJK", "Arial + CJK (merged)",  L"C:\\Windows\\Fonts\\arial.ttf"   },
        };
        static const int s_fontOptionCount = static_cast<int>(sizeof(s_fontOptions) / sizeof(s_fontOptions[0]));

        // Build the list of available fonts once (file existence check).
        static int  s_availableIdx[8]   = {};
        static int  s_availableCount    = 0;
        static bool s_fontsEnumerated   = false;
        if (!s_fontsEnumerated)
        {
            for (int i = 0; i < s_fontOptionCount; ++i)
            {
                if (s_fontOptions[i].primaryPath == nullptr ||
                    GetFileAttributesW(s_fontOptions[i].primaryPath) != INVALID_FILE_ATTRIBUTES)
                {
                    s_availableIdx[s_availableCount++] = i;
                }
            }
            s_fontsEnumerated = true;
        }

        const char* curFamily = UI::GlobalSettings::GetFontFamily();
        int curFamilyAvail = 0;
        for (int i = 0; i < s_availableCount; ++i)
            if (strcmp(s_fontOptions[s_availableIdx[i]].iniKey, curFamily) == 0) { curFamilyAvail = i; break; }

        if (ImGui::BeginCombo("Font Family", s_fontOptions[s_availableIdx[curFamilyAvail]].displayName))
        {
            for (int i = 0; i < s_availableCount; ++i)
            {
                const FontOption& opt = s_fontOptions[s_availableIdx[i]];
                bool selected = (i == curFamilyAvail);
                if (ImGui::Selectable(opt.displayName, selected))
                    UI::GlobalSettings::SetFontFamily(opt.iniKey);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Choose the font used for all overlay text.\n"
                "CJK fonts include Chinese/Japanese and are larger (~5MB atlas).\n"
                "Takes effect immediately.");

        ImGui::Spacing();
        ImGui::SeparatorText("Accent Colors");
        ImGui::TextDisabled("Drive the custom-drawn widgets (toggles, tab strip, panel borders).");
        ImGui::Spacing();

        ColorBar("Accent",        UI::Theme::AccentBasePtr());
        ColorBar("Accent Hover",  UI::Theme::AccentHoverPtr());
        ColorBar("Accent Active", UI::Theme::AccentActivePtr());

        ImGui::Spacing();
        ImGui::SeparatorText("All UI Colors");
        ImGui::TextDisabled("Every ImGui style color used across the modloader and plugin panels.");
        ImGui::Spacing();

        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::BeginChild("##theme_colors", ImVec2(0, 360), true);
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const char* name = ImGui::GetStyleColorName(i);
            if (!name) continue;
            char id[64];
            snprintf(id, sizeof(id), "##col_%d", i);
            ColorBar(id, &style.Colors[i], 220.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted(name);
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
        {
            const wchar_t* iniPath = UI::GlobalSettings::GetIniPath();
            if (iniPath && iniPath[0] != L'\0')
                UI::Theme::SaveColors(iniPath);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to Defaults", ImVec2(160.0f, 0.0f)))
        {
            UI::Theme::ResetColors();
            const wchar_t* iniPath = UI::GlobalSettings::GetIniPath();
            if (iniPath && iniPath[0] != L'\0')
                UI::Theme::SaveColors(iniPath);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Colors apply immediately. Save/Reset write to modloader.ini.");
    }

    static void RenderAboutTab()
    {
        ImGui::Spacing();
        ImGui::TextUnformatted("StarRupture ModLoader By AlienX");
        ImGui::Spacing();
        ImGui::TextUnformatted("  Thanks for using my ModLoader!");
        ImGui::Spacing();
        ImGui::TextDisabled("Build: " MODLOADER_BUILD_TAG);

        char imguiVer[64];
        snprintf(imguiVer, sizeof(imguiVer), "Dear ImGui %s", ImGui::GetVersion());
        ImGui::TextDisabled("%s", imguiVer);

        ImGui::Spacing();
        ImGui::SeparatorText("Loaded Plugins");

        static const PluginInfo* infos[64];
        int count = PluginManager::GetLoadedPluginInfos(infos, 64);
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

    // Called by plugin_manager after each plugin's PluginInit completes so that
    // blocking state is active from the very first key event, not deferred until
    // the user opens the config panel for that plugin.
    void LoadBlockingStateForPlugin(const char* pluginName)
    {
        if (!pluginName || !*pluginName) return;

        wchar_t iniPath[MAX_PATH];
        if (!GetPluginIniPath(pluginName, iniPath, MAX_PATH)) return;

        const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(pluginName);
        if (!schema) return;

        for (int i = 0; i < schema->entryCount; ++i)
        {
            const ConfigEntry& e = schema->entries[i];
            if (e.type != ConfigValueType::Keybind) continue;
            if (!e.section || !e.key) continue;

            // Read the current combo value from INI
            wchar_t wsec[64], wkey[128], wblkKey[128];
            swprintf_s(wsec, L"%S", e.section);
            swprintf_s(wkey, L"%S", e.key);
            swprintf_s(wblkKey, L"%SBlocking", e.key);

            wchar_t comboW[256] = {};
            GetPrivateProfileStringW(wsec, wkey, L"", comboW, ARRAYSIZE(comboW), iniPath);
            if (!comboW[0]) continue;

            char combo[256];
            snprintf(combo, sizeof(combo), "%ls", comboW);

            bool blocking = (GetPrivateProfileIntW(wsec, wblkKey, 0, iniPath) != 0);
            Hooks::Input::SetComboBlocking(combo, blocking);
        }
    }

    void Render(IModLoaderImGui* imgui)
    {
        if (!s_isOpen)
            return;

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(860, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_FirstUseEver,
            ImVec2(0.5f, 0.5f));

        if (!UI::Theme::BeginChamferedWindow("Mod Loader##main", "MOD LOADER", &s_isOpen,
                                              "BUILD " MODLOADER_BUILD_TAG))
            return;

        static const char* s_tabIcons[5] =
        {
            UI::Theme::Icons::Plugins,
            UI::Theme::Icons::Config,
            UI::Theme::Icons::Settings,
            UI::Theme::Icons::Theme,
            UI::Theme::Icons::About,
        };

        const float navSize = 64.0f;
        ImVec2 navStart = ImGui::GetCursorScreenPos();
        s_activeTab = UI::Theme::IconTabBar(s_tabIcons, 5, s_activeTab, navSize, /*vertical=*/true);

        // Vertical divider between the icon column and the tab content,
        // spanning the rest of the window's content area below the header.
        float sepX = navStart.x + navSize + 8.0f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(sepX, navStart.y),
            ImVec2(sepX, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - ImGui::GetStyle().WindowPadding.y),
            ImGui::GetColorU32(ImGuiCol_Separator));

        ImGui::SetCursorScreenPos(ImVec2(sepX + 16.0f, navStart.y));
        ImGui::BeginGroup();
        switch (s_activeTab)
        {
        case 0: RenderPluginsTab();        break;
        case 1: RenderConfigTab(imgui);    break;
        case 2: RenderGlobalSettingsTab(); break;
        case 3: RenderThemeTab();          break;
        case 4: RenderAboutTab();          break;
        default: break;
        }
        ImGui::EndGroup();

        UI::Theme::EndChamferedWindow();
    }
}

#endif // MODLOADER_CLIENT_BUILD
