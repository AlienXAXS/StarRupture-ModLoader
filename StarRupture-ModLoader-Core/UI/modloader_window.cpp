#include "pch.h"
#include "modloader_window.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "plugin_panel_registry.h"
#include "plugins/plugin_manager.h"
#include "config/config_manager.h"
#include "global_settings.h"
#include "theme.h"
#include "update_notice_window.h"
#include "hook_failure_window.h"
#include "hooks/input/keybind_registry.h"
#include "console_window.h"
#include "logging_tab.h"
#include "tick_profiler_window.h"
#include "network_channel/net_timeout.h"
#include "utils/game_thread_dispatch.h"
#ifdef _DEBUG
#include "hooks/game/debug_draw/debug_draw.h"
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <functional>

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
    static bool s_closeRequested = false; // Escape while focused; consumed at the top of Render()
    static bool s_escapeWasDown  = false; // last frame's Escape level, for edge-detection below
    static int  s_selectedPlugin = -1;  // index in Plugins tab

    // Indices into the icon tab strip. Named so a cross-tab jump (the Settings
    // tab's "Open Logging" button) does not hard-code a number that silently
    // points at the wrong tab the next time one is inserted.
    enum : int
    {
        kTabPlugins = 0,
        kTabConfig,
        kTabSettings,
        kTabLogging,
        kTabTheme,
        kTabAbout,
        kTabCount,
    };

    static int  s_activeTab = kTabPlugins;

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

    // PluginManager generation the cached entries were read at, so a reload of the
    // plugin already on screen invalidates them. Starts at a value the counter
    // cannot be on the first frame, so the first render always loads.
    static unsigned s_lastConfigGeneration = static_cast<unsigned>(-1);

    struct RebindState
    {
        bool active           = false;
        bool pendingOpen      = false; // set inside a table; OpenPopup deferred to modal site
        char section[64]      = {};
        char cfgKey[64]       = {};   // the INI key name (not keyboard key)
        char pluginName[64]   = {};
        bool waitingForRelease = false;
        int  heldModifierVk    = 0;   // sided VK of a modifier held alone, 0 if none
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

    // Fire config-change notifications for the in-memory value immediately.
    // Call this at the point kv.value actually changes, for every config
    // type -- not just while a slider is mid-drag -- so plugins see the
    // live value right away instead of waiting for CommitConfigChange to
    // flush it to disk. Deliberately does no disk I/O so it is cheap enough
    // to call every frame a value changes (e.g. once per drag frame).
    static void NotifyConfigChangedLive(const char* pluginName, const ConfigKV& kv)
    {
        UI::PluginPanelRegistry::FireConfigChanged(pluginName, kv.section, kv.key, kv.value);
    }

    // Write a changed value back to disk. Does NOT fire config-change
    // notifications -- callers must call NotifyConfigChangedLive themselves
    // at the point the value changes (this keeps notification timing
    // decoupled from, and not gated on, the disk write).
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
    }

    // -----------------------------------------------------------------------
    // Tab renderers
    // -----------------------------------------------------------------------

    // Small filled dot in front of a plugin's name saying whether it can update
    // itself: green when an auto-update sidecar sits beside the DLL, amber when
    // it does not. A dot rather than a word because it repeats on every row and
    // a column of prose there would swamp the name it is annotating.
    //
    // Drawn over an InvisibleButton rather than as text so it needs no glyph
    // from the icon font and still has a hover rect for the tooltip.
    static void DrawAutoUpdateDot(bool hasManifest)
    {
        const float radius = ImGui::GetFontSize() * 0.26f;
        const float box    = radius * 2.0f;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##autoupd", ImVec2(box, ImGui::GetTextLineHeight()));

        const ImVec4 col = hasManifest ? ImVec4(0.30f, 0.85f, 0.40f, 1.0f)   // green
                                       : ImVec4(1.00f, 0.65f, 0.10f, 1.0f);  // amber
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(origin.x + radius, origin.y + ImGui::GetTextLineHeight() * 0.5f),
            radius, ImGui::GetColorU32(col));

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(hasManifest
                ? "This plugin can Auto Update itself"
                : "This plugin cannot Auto Update itself -- it has no update\n"
                  "manifest, so new versions have to be installed by hand.");

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    }

    // Indexed the same way as RenderPluginsTab's own `statuses[64]` -- true
    // while that row's Unload/Load/Reload is queued or running on the game
    // thread (see PostPluginAction). The row's buttons disable and read
    // "..." for the duration, so a second click can't queue a second
    // Shutdown/Init pass for a plugin whose first one hasn't run yet.
    static bool s_pluginActionPending[64] = {};

    // UNLOAD/LOAD/RELOAD all end up calling PluginShutdown and/or
    // PluginInit (PluginManager::UnloadPlugin/ReloadPlugin), same as the
    // console's own unload/load/reload commands -- which are registered
    // gameThread=true (console_commands.cpp) because a plugin's Shutdown
    // routinely touches engine/UObject state that is only safe to touch
    // from the game thread (see e.g. BetterCheats' player_lookup.h,
    // "Game-thread only -- never from RenderImGui"). This window's Render()
    // runs from inside the D3D Present hook, i.e. the render thread, so
    // calling PluginManager directly from a button handler here has the
    // same exposure the console path already avoids. Posting through
    // GameThreadDispatch, exactly like Dispatch() does for a gameThread
    // command, gives these buttons the same guarantee.
    static void PostPluginAction(int index, std::function<void(int)> action)
    {
        s_pluginActionPending[index] = true;
        GameThreadDispatch::PostVoid([index, action]()
        {
            action(index);
            s_pluginActionPending[index] = false;
        });
    }

    static void RenderPluginsTab()
    {
        // The return is the TOTAL record count, not how many were copied --
        // clamp to the buffer before iterating.
        static PluginManager::PluginStatus statuses[64];
        const int total = PluginManager::GetAllPluginStatuses(statuses, 64);
        const int count = total < 64 ? total : 64;

        if (count == 0)
        {
            ImGui::TextDisabled("No plugins found.");
            return;
        }

        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##plugins", 5, tableFlags))
        {
            // All columns are stretch-weighted (not fixed) so dragging a
            // header divider adjusts the *ratio* between columns, and the
            // ratios hold when the window is resized. ImGui persists the
            // user's weights per-table in modloader_imgui.ini automatically.
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Author",  ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Status",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < count; ++i)
            {
                const PluginManager::PluginStatus& s = statuses[i];
                ImGui::TableNextRow();
                if (s.isLoaded)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(UI::Theme::AccentColorVec4(0.10f)));

                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                DrawAutoUpdateDot(s.hasUpdateManifest);
                ImGui::PopID();
                ImGui::TextUnformatted(s.name[0] ? s.name : "?");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(s.version[0] ? s.version : "?");

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(s.author[0] ? s.author : "?");

                ImGui::TableSetColumnIndex(3);
                if (s.isWrongTarget)
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Cannot Load");
                else if (s.hookScanFailed)
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Hooks Failed");
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
                else if (s.hookScanFailed)
                {
                    // The popup has already been dismissed by the time anyone
                    // comes looking here, so this reopens it rather than trying
                    // to restate a multi-line report inside a table cell.
                    if (ImGui::Button("WHY?"))
                        UI::HookFailureWindow::Show();
                    ImGui::SameLine();
                    ImGui::TextDisabled("A hook it needs could not be found in this game build");
                }
                else if (s.isOutOfDate)
                {
                    ImGui::TextDisabled(s.needsModLoaderUpdate
                        ? "Update The Mod Loader"
                        : "Please Update The Plugin");
                }
                else
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));

                    const bool busy = s_pluginActionPending[i];

                    // Unload — active only when loaded, and not mid-action
                    if (!s.isLoaded || busy) ImGui::BeginDisabled();
                    if (ImGui::Button(busy ? "..." : "UNLOAD"))
                        PostPluginAction(i, [](int idx) { PluginManager::UnloadPlugin(idx); });
                    if (!s.isLoaded || busy) ImGui::EndDisabled();

                    ImGui::SameLine();

                    // Load — active only when unloaded, and not mid-action
                    if (s.isLoaded || busy) ImGui::BeginDisabled();
                    if (ImGui::Button(busy ? "..." : "LOAD"))
                        PostPluginAction(i, [](int idx) { PluginManager::ReloadPlugin(idx); });
                    if (s.isLoaded || busy) ImGui::EndDisabled();

                    ImGui::SameLine();

                    // Reload — always active when idle; accented as the
                    // primary action.
                    if (busy) ImGui::BeginDisabled();
                    ImGui::PushStyleColor(ImGuiCol_Button, UI::Theme::AccentColorVec4(0.20f));
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::AccentColorVec4(1.0f));
                    if (ImGui::Button(busy ? "..." : "RELOAD"))
                        PostPluginAction(i, [](int idx) { PluginManager::ReloadPlugin(idx); });
                    ImGui::PopStyleColor(2);
                    if (busy) ImGui::EndDisabled();

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
    //   Col 0 (Label)   -- setting name, wrapped to labelColWidth
    //   Col 1 (Widget)  -- the editable control (empty for booleans -- their
    //                      toggle lives in Col 2, see below)
    //   Col 2 (Actions) -- blocking toggle (keybind only) + reset button,
    //                      reset always anchored to the column's right edge
    //                      so it lines up on every row regardless of what
    //                      else Col 2 is showing
    //   ...then, on a row with a description, a second line directly below
    //   the label spanning Col 0 + Col 1 (up to where Col 2/actions starts)
    //   as dimmed wrapped text -- Col 0 is NoClip (RenderConfigTab) so this
    //   is allowed to draw past its own column's edge instead of being cut
    //   off at it. Every entry type gets this treatment (no more hover-only
    //   marquee).
    //
    // Col 0/1/2's *first* line is vertically centered against topRowH -- the
    // taller of the label and a control's frame height -- so the widget,
    // Block toggle and reset always sit level with the label, whether or
    // not this row also has a description line underneath.
    static void RenderConfigEntry(ConfigKV& kv, const ConfigEntry* e, const char* pluginName,
                                   float labelColWidth, float actionsW)
    {
        ImGui::TableNextRow();

        char id[128];
        snprintf(id, sizeof(id), "##%s_%s", kv.section, kv.key);

        bool showReset = e && e->defaultValue && e->defaultValue[0] &&
                         !(strcmp(kv.section, "General") == 0 && strcmp(kv.key, "Enabled") == 0);

        const bool  isBool    = e && e->type == ConfigValueType::Boolean;
        const bool  isKeybind = e && e->type == ConfigValueType::Keybind;
        const bool  hasDesc   = e && e->description && e->description[0];
        const float fh        = ImGui::GetFrameHeight();
        const float spacingY  = ImGui::GetStyle().ItemSpacing.y;
        const float rowTopY   = ImGui::GetCursorPosY();

        // Measure where Col 2 starts before drawing anything -- the
        // description line (below) wraps to that x, spanning Col 0 + Col 1
        // without depending on Col 1's own stretch width. TableSetColumnIndex
        // supports being visited out of order for exactly this.
        ImGui::TableSetColumnIndex(2);
        const float col2X = ImGui::GetCursorPosX();
        ImGui::TableSetColumnIndex(0);
        const float descWrapWidth = col2X - ImGui::GetCursorPosX();

        const float labelH  = ImGui::CalcTextSize(kv.key, nullptr, false, labelColWidth).y;
        const float topRowH = labelH > fh ? labelH : fh;

        // ---- Col 0: label ---------------------------------------------------
        ImGui::TableSetColumnIndex(0);
        ImGui::SetCursorPosY(rowTopY + (topRowH - labelH) * 0.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + labelColWidth);
        ImGui::TextUnformatted(kv.key);
        ImGui::PopTextWrapPos();

        // ---- Col 1: widget -----------------------------------------------
        ImGui::TableSetColumnIndex(1);
        ImGui::SetCursorPosY(rowTopY + (topRowH - fh) * 0.5f);
        ImGui::SetNextItemWidth(-FLT_MIN); // fill the column

        bool widgetHovered = false;

        if (isBool)
        {
            // Nothing here -- the toggle lives in Col 2 next to the reset
            // button, and the description already moved under the label.
        }
        else if (e && e->type == ConfigValueType::Integer)
        {
            int ival = atoi(kv.value);
            bool hasRange = e->rangeMax > e->rangeMin;
            if (hasRange)
            {
                if (ImGui::SliderInt(id, &ival, (int)e->rangeMin, (int)e->rangeMax))
                {
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                    NotifyConfigChangedLive(pluginName, kv);
                }
                if (ImGui::IsItemDeactivated())
                    CommitConfigChange(pluginName, kv);
            }
            else
            {
                if (ImGui::InputInt(id, &ival, 1, 10))
                {
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                    NotifyConfigChangedLive(pluginName, kv);
                    CommitConfigChange(pluginName, kv);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    snprintf(kv.value, sizeof(kv.value), "%d", ival);
                    NotifyConfigChangedLive(pluginName, kv);
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
                {
                    FormatFloat(kv.value, sizeof(kv.value), fval);
                    NotifyConfigChangedLive(pluginName, kv);
                }
                if (ImGui::IsItemDeactivated())
                    CommitConfigChange(pluginName, kv);
            }
            else
            {
                if (ImGui::InputFloat(id, &fval, 0.0f, 0.0f, "%.6f"))
                {
                    FormatFloat(kv.value, sizeof(kv.value), fval);
                    NotifyConfigChangedLive(pluginName, kv);
                    CommitConfigChange(pluginName, kv);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    FormatFloat(kv.value, sizeof(kv.value), fval);
                    NotifyConfigChangedLive(pluginName, kv);
                    CommitConfigChange(pluginName, kv);
                }
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else if (isKeybind)
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
                s_rebind.heldModifierVk    = 0;
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }
        else
        {
            // String or unknown schema entry: plain text input.
            if (ImGui::InputText(id, kv.value, sizeof(kv.value),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                NotifyConfigChangedLive(pluginName, kv);
                CommitConfigChange(pluginName, kv);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                NotifyConfigChangedLive(pluginName, kv);
                CommitConfigChange(pluginName, kv);
            }
            widgetHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        }

        if (hasDesc && widgetHovered)
            ImGui::SetTooltip("%s", e->description);

        // ---- Col 2: actions ------------------------------------------------
        ImGui::TableSetColumnIndex(2);
        const float colStartX = col2X;
        const float colY      = rowTopY + (topRowH - fh) * 0.5f;
        ImGui::SetCursorPosY(colY);

        // Boolean toggle lives here (not in Col 1) so Col 1 can stay empty.
        if (isBool)
        {
            // Accept the same spellings ConfigReadBool does, but always write back
            // "1"/"0": that is what ConfigWriteBool and the schema default writer emit,
            // and plugins that read booleans via ReadInt/ReadString and compare against
            // "1" break when this toggle is the one path that writes "true"/"false".
            bool bval = (_stricmp(kv.value, "true") == 0 ||
                         _stricmp(kv.value, "yes") == 0 ||
                         strcmp(kv.value, "1") == 0);
            char lbl[128];
            snprintf(lbl, sizeof(lbl), "##chk%s", id);
            if (UI::Theme::ToggleSwitch(lbl, &bval))
            {
                strncpy_s(kv.value, bval ? "1" : "0", _TRUNCATE);
                NotifyConfigChangedLive(pluginName, kv);
                CommitConfigChange(pluginName, kv);
            }
        }

        // Blocking toggle (keybind rows only) -- given a short visible label,
        // not just a hover tooltip: unlabeled, it previously gave no visible
        // hint at all that there was a second control here.
        if (isKeybind)
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
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Block");
            ImGui::SameLine();
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
        }

        // Reset button -- always anchored to the same x (the column's right
        // edge minus its own width), so it lines up on every row regardless
        // of whether a toggle/label preceded it in this column.
        if (showReset)
        {
            const float resetW = fh;
            ImGui::SetCursorPos(ImVec2(colStartX + actionsW - resetW, colY));
            char resetId[176];
            snprintf(resetId, sizeof(resetId), "R##r_%s_%s", kv.section, kv.key);
            if (ImGui::Button(resetId, ImVec2(resetW, fh)))
            {
                strncpy_s(kv.value, e->defaultValue, _TRUNCATE);
                NotifyConfigChangedLive(pluginName, kv);
                CommitConfigChange(pluginName, kv);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Reset to default: %s", e->defaultValue);
        }

        // ---- Description: its own line under the label, spanning Col 0 +
        // Col 1 (Col 0 is NoClip -- see RenderConfigTab) instead of being
        // squeezed into the label column alone, so a short description
        // stays on one line and only a genuinely long one wraps.
        if (hasDesc)
        {
            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosY(rowTopY + topRowH + spacingY);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + descWrapWidth);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted(e->description);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
    }

    // Returns true if the given VK is a modifier key (Ctrl/Shift/Alt left or right).
    static bool IsModifierVK(int vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
               vk == VK_LMENU    || vk == VK_RMENU;
    }

    // Writes comboStr as the new bind for the entry being captured and closes
    // the popup. Shared by the non-modifier scan and the bare-modifier path
    // below, so "Ctrl+F5" and a plain "LeftShift" commit through the same code.
    static void CommitRebindCombo(const char* comboStr)
    {
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
                NotifyConfigChangedLive(s_rebind.pluginName, kv);
                CommitConfigChange(s_rebind.pluginName, kv, oldValue, schEntry);
                break;
            }
        }

        s_rebind.active        = false;
        s_rebind.heldModifierVk = 0;
        ImGui::CloseCurrentPopup();
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
                s_rebind.active        = false;
                s_rebind.heldModifierVk = 0;
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
                CommitRebindCombo(comboStr);
                break;
            }

            // A modifier pressed and released with nothing else pressed in
            // between is itself a valid bind (e.g. "LeftShift") -- the loop
            // above already claims any non-modifier press, so reaching here
            // with s_rebind still active means no combo fired this frame.
            // Tracked by its sided VK (not curMods, which is unsided) so the
            // captured name matches what ResolveSidedVK reports at dispatch
            // time. Two modifiers held together with nothing else is not a
            // combo this picker supports; the first one seen wins.
            if (s_rebind.active)
            {
                int downModifierVk = 0;
                for (int vk : {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU})
                {
                    if (GetAsyncKeyState(vk) & 0x8000) { downModifierVk = vk; break; }
                }

                if (downModifierVk)
                {
                    s_rebind.heldModifierVk = downModifierVk;
                }
                else if (s_rebind.heldModifierVk)
                {
                    EModKey mk = Hooks::Input::VKToModKey(s_rebind.heldModifierVk);
                    s_rebind.heldModifierVk = 0;
                    if (mk != EModKey::Unknown)
                    {
                        char comboStr[64];
                        Hooks::Input::FormatComboString(mk, EModKeyMod_None, comboStr, sizeof(comboStr));
                        CommitRebindCombo(comboStr);
                    }
                }
            }

            ImGui::EndPopup();
        }
        else
        {
            // Popup was closed externally.
            s_rebind.active        = false;
            s_rebind.heldModifierVk = 0;
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

        // Left panel: plugin list (user-resizable via the splitter below)
        static float s_cfgListWidth = 200.0f;
        const float totalWidth = ImGui::GetContentRegionAvail().x;
        const float minListWidth = 100.0f;
        const float minEditorWidth = 150.0f;
        if (s_cfgListWidth > totalWidth - minEditorWidth)
            s_cfgListWidth = totalWidth - minEditorWidth;
        if (s_cfgListWidth < minListWidth)
            s_cfgListWidth = minListWidth;

        ImGui::BeginChild("##cfg_list", ImVec2(s_cfgListWidth, 0), true);
        for (int i = 0; i < count; ++i)
        {
            const char* name = infos[i]->name ? infos[i]->name : "?";
            bool selected = (s_selectedPlugin == i);
            if (ImGui::Selectable(name, selected))
                s_selectedPlugin = i;
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);

        // Draggable splitter between the plugin list and the config editor.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::Button("##cfg_splitter", ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
            s_cfgListWidth += ImGui::GetIO().MouseDelta.x;
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 0.0f);

        // Right panel: config editor
        ImGui::BeginChild("##cfg_editor", ImVec2(0, 0), false);

        if (s_selectedPlugin < 0 || s_selectedPlugin >= count)
        {
            ImGui::TextDisabled("Select a plugin on the left.");
        }
        else
        {
            const PluginInfo* info = infos[s_selectedPlugin];

            // Rebuild the cache when the selection changes OR when any plugin has
            // been loaded/unloaded/reloaded since we last read the file.
            //
            // Selection alone was not enough: reloading the plugin you are already
            // looking at changes no index, so the editor kept showing the .ini as
            // parsed the first time it was opened. A setting added to the schema in
            // the new build stayed invisible until you clicked to another plugin and
            // back, which reads exactly like the loader ignoring the new build.
            const unsigned generation = PluginManager::GetPluginGeneration();

            if (s_lastConfigPlugin != s_selectedPlugin || s_lastConfigGeneration != generation)
            {
                s_lastConfigPlugin     = s_selectedPlugin;
                s_lastConfigGeneration = generation;
                LoadConfigEntries(info->name);
            }

            // One indent for every section on this page, "Plugin Tools"
            // included, so its heading lines up with "Drone"/"Interaction"/etc.
            // instead of sitting flush with the window edge while they sit
            // inside a table's own padding.
            const float kSectionIndent = 8.0f;

            // Plugin panels button row (shown before config entries)
            ImGui::Indent(kSectionIndent);
            ImGui::SeparatorText("Plugin Tools");
            UI::PluginPanelRegistry::RenderPanelButtons(imgui, info->name);
            ImGui::Unindent(kSectionIndent);
            ImGui::Spacing();

            if (s_configEntries.empty())
            {
                ImGui::TextDisabled("No config file found for this plugin.");
            }
            else
            {
                const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(info->name);

                ImGui::TextDisabled("Changes are saved immediately.");
                ImGui::Spacing();

                const float fh      = ImGui::GetFrameHeight();
                const float spacing = ImGui::GetStyle().ItemSpacing.x;

                // Actions column: sized to fit its widest possible content --
                // a keybind row's "Block" label plus its toggle -- with the
                // reset button always anchored to the column's right edge
                // (RenderConfigEntry) so it lines up on every row regardless
                // of whether a toggle/label came before it in this column.
                const float toggleW     = UI::Theme::ToggleSwitchSize().x;
                const float blockLabelW = ImGui::CalcTextSize("Block").x;
                const float resetW      = fh;
                const float actionsW    = blockLabelW + spacing + toggleW + spacing + resetW;

                // Label column: fits the widest label on this plugin's page --
                // uncapped by a fixed constant, so a label only wraps
                // (RenderConfigEntry) when the window genuinely doesn't have
                // room for it, capped instead by what's actually left over
                // once the actions column and a usable minimum for the
                // widget column are reserved.
                const float kLabelColMin  = 130.0f;
                const float kMinWidgetCol = 160.0f;
                float labelColWidth = kLabelColMin;
                for (const auto& kv : s_configEntries)
                {
                    const float w = ImGui::CalcTextSize(kv.key).x;
                    if (w > labelColWidth) labelColWidth = w;
                }
                labelColWidth += ImGui::GetStyle().CellPadding.x; // breathing room before the wrap point

                const float maxLabelColWidth = ImGui::GetContentRegionAvail().x - actionsW - kMinWidgetCol;
                if (labelColWidth > maxLabelColWidth && maxLabelColWidth > kLabelColMin)
                    labelColWidth = maxLabelColWidth;

                // One cell padding for every section table on this page.
                const ImVec2 kCellPadding(8.0f, 8.0f);

                // One 3-column table per section so separators span full width
                // and all rows within a section share the same column edges.
                //   Col 0  Label   -- fixed, sized above
                //   Col 1  Widget  -- stretches to fill remaining space
                //   Col 2  Actions -- fixed (blocking toggle + reset)
                const ImGuiTableFlags tblFlags =
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_PadOuterX;

                const char* curSection = nullptr;
                bool        tableOpen  = false;

                // Closes whatever section table is currently open, undoing
                // the indent/padding pushed when it was opened below. Called
                // both between sections and after the last one.
                auto closeSectionTable = [&]()
                {
                    if (!tableOpen) return;
                    ImGui::EndTable();
                    ImGui::PopStyleVar(); // CellPadding, pushed when the table opened
                    ImGui::Unindent(kSectionIndent);
                    tableOpen = false;
                };

                for (auto& kv : s_configEntries)
                {
                    if (!curSection || strcmp(curSection, kv.section) != 0)
                    {
                        closeSectionTable();
                        if (curSection) ImGui::Spacing();

                        ImGui::Indent(kSectionIndent);
                        ImGui::SeparatorText(kv.section);
                        curSection = kv.section;

                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, kCellPadding);
                        char tblId[128];
                        snprintf(tblId, sizeof(tblId), "##cfg_%s", kv.section);
                        if (ImGui::BeginTable(tblId, 3, tblFlags))
                        {
                            // NoClip: a description longer than the label column
                            // (RenderConfigEntry) is deliberately allowed to draw
                            // past this column's right edge, into Col 1's space,
                            // instead of being clipped at it.
                            ImGui::TableSetupColumn("##lbl",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoClip, labelColWidth);
                            ImGui::TableSetupColumn("##widget", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("##acts",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, actionsW);
                            tableOpen = true;
                        }
                        else
                        {
                            // Table fully clipped (e.g. scrolled out) -- undo
                            // what was pushed above since closeSectionTable()
                            // won't run for a table that never opened.
                            ImGui::PopStyleVar();
                            ImGui::Unindent(kSectionIndent);
                        }
                    }

                    if (tableOpen)
                    {
                        const ConfigEntry* entry = FindSchemaEntry(schema, kv.section, kv.key);
                        RenderConfigEntry(kv, entry, info->name, labelColWidth, actionsW);
                    }
                }

                closeSectionTable();
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

        bool showDebug = UI::GlobalSettings::GetShowDebugValues();
        if (UI::Theme::ToggleSwitch("ModLoader Debug Values", &showDebug))
            UI::GlobalSettings::SetShowDebugValues(showDebug);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Adds the loader's own bookkeeping to the HUD box: which plugins hold\n"
                "input tokens, what input arbitration resolves to this frame, and what\n"
                "ImGui thinks it owns.\n\n"
                "Turn this on if the game stops responding to input while a plugin is\n"
                "loaded. A token still held with no UI on screen is a leak, and this\n"
                "names the plugin responsible.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Networking");
        ImGui::Spacing();

        // -1 means "not read yet". The slider owns the value from then on; the
        // commit below is what writes it back.
        static int s_connTimeout = -1;
        if (s_connTimeout < 0)
            s_connTimeout = static_cast<int>(NetTimeout::GetConnectionTimeout());

        // A literal with no format specifier is a legal ImGui slider format, and
        // is how 0 reads as what it means rather than as "0 seconds" -- which
        // would suggest an instant timeout, the opposite of what it does.
        const char* timeoutFmt = (s_connTimeout == 0) ? "Engine default" : "%d s";

        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("Connection Timeout", &s_connTimeout, 0, 900, timeoutFmt,
                         ImGuiSliderFlags_AlwaysClamp);

        // Committed on release, not per frame: SetConnectionTimeout persists to
        // modloader.ini, and a drag would otherwise write the file every frame.
        if (ImGui::IsItemDeactivatedAfterEdit())
            NetTimeout::SetConnectionTimeout(static_cast<float>(s_connTimeout));

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "How long a peer may go silent before the connection is dropped.\n\n"
                "Raise this if players are disconnected while the host loads a map:\n"
                "the host's game thread does not tick during a load, so nothing is\n"
                "sent -- not even keepalives -- and the client sees silence for the\n"
                "whole load.\n\n"
                "BOTH ends need the mod loader for this to help. Each side times the\n"
                "other out against its own clock, so the side that gives up is the\n"
                "side that needs the higher value.\n\n"
                "The cost: a genuinely dead connection now holds its player slot for\n"
                "this long instead of a minute.\n\n"
                "0 leaves the engine's own default alone. Applies on the next tick;\n"
                "persisted to modloader.ini.");
        }

        // What the live net driver actually holds -- not what is configured. If
        // someone drops anyway, this is the number the engine timed them out on.
        const float appliedTimeout = NetTimeout::GetAppliedConnectionTimeout();
        if (appliedTimeout > 0.0f)
            ImGui::TextDisabled("Active on the current net driver: %.0f s", appliedTimeout);
        else
            ImGui::TextDisabled("No net driver right now (not in a session).");

        ImGui::Spacing();
        ImGui::SeparatorText("Auto Update");
        ImGui::Spacing();

        // Read the INI once -- the toggle then owns the value and writes back
        // on change. The updater itself re-reads the INI at next startup.
        static bool s_autoUpdateEnabled = []
        {
            const wchar_t* iniPath = UI::GlobalSettings::GetIniPath();
            if (!iniPath || iniPath[0] == L'\0')
                return true;
            return GetPrivateProfileIntW(L"AutoUpdate", L"Enabled", 1, iniPath) != 0;
        }();

        if (UI::Theme::ToggleSwitch("Enable Auto Updates", &s_autoUpdateEnabled))
        {
            const wchar_t* iniPath = UI::GlobalSettings::GetIniPath();
            if (iniPath && iniPath[0] != L'\0')
                WritePrivateProfileStringW(L"AutoUpdate", L"Enabled",
                    s_autoUpdateEnabled ? L"1" : L"0", iniPath);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Controls the modloader update check and plugin auto-updates.\n"
                               "Update checks run at game startup, so this takes effect on the\n"
                               "next launch. Persisted to modloader.ini.");

        ImGui::Spacing();
        ImGui::SeparatorText("Logging");
        ImGui::Spacing();

        ImGui::TextDisabled("Log levels moved to their own tab, which can also set a level per plugin.");
        if (ImGui::Button("Open Logging"))
            s_activeTab = kTabLogging;

        ImGui::Spacing();
        ImGui::SeparatorText("Developer");
        ImGui::Spacing();

        ImGui::TextDisabled("Developer console: press %s to open.",
                            UI::ConsoleWindow::GetOpenKeyName());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open"))
            UI::ConsoleWindow::Open();

        ImGui::Spacing();
        ImGui::SeparatorText("Diagnostics");
        ImGui::Spacing();

        if (ImGui::Button("Open Tick Profiler"))
            UI::TickProfilerWindow::Open();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Live view of recent tick times and a breakdown of every recorded stutter\n"
                               "(engine tick, dispatch drain, per-plugin callback cost, optional stack\n"
                               "sampling). Stutter logging/sampling toggles live in that window now.");

        ImGui::Spacing();
        ImGui::TextDisabled("Settings are saved to modloader.ini immediately.");

#ifdef _DEBUG
        ImGui::Spacing();
        ImGui::SeparatorText("Debug");
        ImGui::Spacing();

        if (ImGui::Button("Test Plugin Update Notice"))
            UI::UpdateNoticeWindow::PopulateTestData();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Populates the plugin-updated popup with fake\n"
                              "entries and opens it. Debug builds only.");

        if (ImGui::Button("Test Hook Failure Notice"))
            UI::HookFailureWindow::PopulateTestData();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Populates the failed-hooks popup with fake\n"
                              "entries and opens it. Debug builds only.");

        if (ImGui::Button("Crash Game (YOLO)"))
        {
            // Deliberate null-pointer write -- raises a real SEH access violation
            // so it flows through the engine's crash reporting thread exactly
            // like an organic crash, exercising the CrashReporter hook.
            volatile int* crashPtr = nullptr;
            *crashPtr = 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Immediately crashes the game with a null pointer write, to\n"
                              "verify the CrashReporter hook shows its own dialog instead of\n"
                              "spawning CrashReportClient.exe. Debug builds only.");

        // Debug draw exercises hooks->HUD->DebugDraw. This runs on the render
        // thread (ImGui is drawn from the Present hook), so both actions have
        // to be handed to the game thread before they touch the line batchers.
        if (ImGui::Button("Test Debug Draw"))
            GameThreadDispatch::PostVoid([]() { Hooks::DebugDraw::DrawTestScene(); });
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draws one of every in-world debug primitive in a labelled row\n"
                              "roughly 7m in front of your character. Stays until you press\n"
                              "Clear Debug Draw. Needs a loaded world with a pawn.\n"
                              "Debug builds only.");

        ImGui::SameLine();
        if (ImGui::Button("Clear Debug Draw"))
            GameThreadDispatch::PostVoid([]()
            {
                Hooks::DebugDraw::FlushPersistentLines();
                Hooks::DebugDraw::ClearAllStrings();
            });
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clears both persistent line batchers and every world-anchored\n"
                              "debug string, for every plugin -- not just the test scene.");
#endif
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

        // Slide over a discrete step index rather than the raw percent -- SliderInt always
        // yields whole integers, so the grab is genuinely limited to these stops while
        // dragging instead of only snapping once the mouse stops moving.
        static const int s_minPercent  = 75;
        static const int s_stepPercent = 25;
        static const int s_maxPercent  = 500;
        static const int s_stepCount   = (s_maxPercent - s_minPercent) / s_stepPercent;

        int curIdx = static_cast<int>(roundf((UI::GlobalSettings::GetFontScale() * 100.0f - s_minPercent) / s_stepPercent));
        if (curIdx < 0)           curIdx = 0;
        if (curIdx > s_stepCount) curIdx = s_stepCount;

        char idxLabel[16];
        snprintf(idxLabel, sizeof(idxLabel), "%d%%", s_minPercent + curIdx * s_stepPercent);

        if (ImGui::SliderInt("Text Size", &curIdx, 0, s_stepCount, idxLabel))
            UI::GlobalSettings::SetFontScale((s_minPercent + curIdx * s_stepPercent) / 100.0f);

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scales all ImGui window text. Takes effect immediately.");

        if (s_minPercent + curIdx * s_stepPercent >= s_maxPercent)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), "Castle56 Mode Enabled");
        }

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

        // Seed the edge-detector with whatever Escape is doing right now, so
        // opening the window on a keypress that happens to leave Escape held
        // (or opening it programmatically while the player is mid-press for
        // some unrelated reason) doesn't read as a fresh Escape next frame
        // and instantly close the window it just opened.
        if (s_isOpen)
            s_escapeWasDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
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

        // Deferred by a frame (see the Escape check below): closing here, not
        // inside the same keypress that requested it, means input capture --
        // already decided for this frame before Render runs -- can't flip out
        // from under Escape's own WM_KEYUP. Checked before Begin so closing
        // just means skipping Begin/End entirely, same as the window never
        // having opened this frame.
        if (s_closeRequested)
        {
            s_closeRequested = false;
            Toggle(); // s_isOpen is true here (guarded above), so this closes it -- same path the open/close keybind uses
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(860, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_FirstUseEver,
            ImVec2(0.5f, 0.5f));

        // NoScrollbar on the outer window -- the title bar (drawn by
        // BeginChamferedWindow) must stay fixed, so scrolling happens only
        // inside the nav column and tab content children below, each
        // independently, rather than the whole window scrolling as one unit.
        if (!UI::Theme::BeginChamferedWindow("Mod Loader##main", "MOD LOADER", &s_isOpen,
                                              "BUILD " MODLOADER_BUILD_TAG, ImGuiWindowFlags_NoScrollbar))
            return;

        // Snapshot before the tabs render below: RenderConfigTab -> Render-
        // RebindModal can cancel an in-progress capture this same frame, which
        // flips s_rebind.active to false partway through. Reading the live
        // value in the Escape check further down would let the very press
        // that just cancelled a capture also close the window -- this keeps
        // that check looking at whether a capture was active when the frame
        // started, not whether one is still active by the time it finishes.
        const bool wasCapturingThisFrame = s_rebind.active;

        static const char* s_tabIcons[kTabCount] =
        {
            UI::Theme::Icons::Plugins,
            UI::Theme::Icons::Config,
            UI::Theme::Icons::Settings,
            UI::Theme::Icons::Logging,
            UI::Theme::Icons::Theme,
            UI::Theme::Icons::About,
        };

        // Drawn under the icon -- one short word each so they fit the nav
        // column at the default font scale. The sentence explaining what the
        // tab actually does lives in the tooltip below.
        static const char* s_tabLabels[kTabCount] =
        {
            "Plugins",
            "Config",
            "Settings",
            "Logging",
            "Theme",
            "About",
        };
        static const char* s_tabTooltips[kTabCount] =
        {
            "Plugins -- load, unload and reload installed plugins",
            "Plugin Config -- edit each plugin's own settings and keybinds",
            "Settings -- HUD overlays, auto-update and diagnostics",
            "Logging -- log levels for the loader, the game and each plugin",
            "Theme -- font, text size and every UI color",
            "About -- build tag and the plugins currently loaded",
        };

        const float navSize  = 64.0f;
        const float navWidth = navSize + 8.0f;
        ImVec2 avail    = ImGui::GetContentRegionAvail();
        ImVec2 navStart = ImGui::GetCursorScreenPos();

        ImGui::BeginChild("##nav_col", ImVec2(navWidth, avail.y), false);
        s_activeTab = UI::Theme::IconTabBar(s_tabIcons, kTabCount, s_activeTab, navSize,
                                            /*vertical=*/true, s_tabLabels, s_tabTooltips);
        ImGui::EndChild();

        // Vertical divider between the icon column and the tab content,
        // spanning the height shared by both child regions.
        float sepX = navStart.x + navWidth;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(sepX, navStart.y),
            ImVec2(sepX, navStart.y + avail.y),
            ImGui::GetColorU32(ImGuiCol_Separator));

        ImGui::SetCursorScreenPos(ImVec2(sepX + 16.0f, navStart.y));
        ImGui::BeginChild("##tab_content", ImVec2(0, avail.y), false);
        switch (s_activeTab)
        {
        case kTabPlugins:  RenderPluginsTab();        break;
        case kTabConfig:   RenderConfigTab(imgui);    break;
        case kTabSettings: RenderGlobalSettingsTab(); break;
        case kTabLogging:  UI::LoggingTab::Render();  break;
        case kTabTheme:    RenderThemeTab();          break;
        case kTabAbout:    RenderAboutTab();          break;
        default: break;
        }
        ImGui::EndChild();

        // Escape closes the window whenever it's open -- deliberately not
        // gated on ImGui focus. With several loader/plugin windows open at
        // once (e.g. two plugin panels together), the one that should close
        // is often not the currently-focused one, and a focus gate here just
        // means Escape does nothing. The only thing that still holds it back
        // is the rebind picker: if it was capturing when this frame started
        // (wasCapturingThisFrame above), Escape already meant "cancel the
        // capture" there, and that same press must not also close the
        // window. The close itself is deferred to next frame; see the top
        // of Render().
        //
        // Also edge-triggered, not level-triggered: GetAsyncKeyState reports
        // Escape held down for every frame of one physical press, not just
        // the first. s_escapeWasDown is updated every frame the window
        // renders, even while the picker is active, so the same press that
        // just cancelled a capture reads as still-held (not a fresh press)
        // on every later frame too, instead of closing the window the first
        // moment s_rebind.active catches up to false.
        const bool escapeDownNow = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool escapePressed = escapeDownNow && !s_escapeWasDown;
        s_escapeWasDown = escapeDownNow;

        if (escapePressed && !wasCapturingThisFrame)
        {
            s_closeRequested = true;
        }

        UI::Theme::EndChamferedWindow();
    }
}

#endif // MODLOADER_CLIENT_BUILD
