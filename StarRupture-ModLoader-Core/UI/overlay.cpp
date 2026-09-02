#include "pch.h"
#include "overlay.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "plugins/plugin_manager.h"
#include "global_settings.h"
#include "plugin_panel_registry.h"
#include "console_window.h"
#include "modloader_window.h"
#include "tick_profiler_window.h"
#include "update_notice_window.h"
#include "hook_failure_window.h"
#include "../core/client_ui.h"
#include "../hooks/input/keybind_registry.h"
#include <cstdio>
#include <cstring>

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "local-build"
#endif

namespace UI::Overlay
{
    static char s_openKeyName[16] = "F2";
    static bool s_visible         = true;

    void SetOpenKeyName(const char* keyName)
    {
        if (keyName)
            strncpy_s(s_openKeyName, keyName, _TRUNCATE);
    }

    void SetVisible(bool visible)
    {
        s_visible = visible;
    }

    void Render()
    {
        if (!s_visible)
            return;

        ImGuiIO& io = ImGui::GetIO();

        // Bottom-left corner, 10 px from edges
        const float PAD = 10.0f;
        ImGui::SetNextWindowPos(ImVec2(PAD, io.DisplaySize.y - PAD),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::SetNextWindowSize(ImVec2(0, 0));

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoInputs;

        if (ImGui::Begin("##modloader_overlay", nullptr, flags))
        {
            int pluginCount = PluginManager::GetLoadedPluginCount();
            ImGui::Text("AlienX's Mod Loader %s | %d Plugin%s Loaded | %s To Open UI",
                        MODLOADER_BUILD_TAG,
                        pluginCount,
                        pluginCount == 1 ? "" : "s",
                        s_openKeyName);

            if (UI::GlobalSettings::GetUpdateAvailable())
            {
				ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Mod Loader update available -- please update!");
            }
        }
        ImGui::End();
    }

    // -----------------------------------------------------------------------
    // Debug values
    //
    // Everything here answers one question: "is the modloader the reason the
    // game is not responding?" That question was previously unanswerable from
    // inside the game -- input arbitration is driven by booleans nobody can see,
    // and a plugin that acquires an input token and fails to release it leaves
    // the player mute with nothing on screen to explain it.
    //
    // Live values only, no history: this is a HUD, and anything worth keeping
    // belongs in the log.
    // -----------------------------------------------------------------------
    static void RenderDebugValues()
    {
        const ImVec4 kBad  { 1.00f, 0.45f, 0.35f, 1.0f };
        const ImVec4 kWarn { 1.00f, 0.80f, 0.30f, 1.0f };
        const ImVec4 kOk   { 0.55f, 0.85f, 0.55f, 1.0f };

        ImGui::Separator();
        ImGui::TextDisabled("ModLoader Debug");

        UI::PluginPanelRegistry::InputTokenSummary tokens{};
        UI::PluginPanelRegistry::GetInputTokenSummary(&tokens);

        // The token counts, with the holders named. Coloured because the count
        // being non-zero is not itself a problem -- it is only a problem when
        // nothing is on screen to justify it, and the reader needs to be pulled
        // to the line rather than have to go looking for it.
        ImGui::TextColored(tokens.captureCount > 0 ? kWarn : kOk,
                           "Input capture tokens: %d", tokens.captureCount);
        if (tokens.captureCount > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", tokens.captureOwners);
        }

        ImGui::TextColored(tokens.passthroughCount > 0 ? kWarn : kOk,
                           "Input passthrough tokens: %d", tokens.passthroughCount);
        if (tokens.passthroughCount > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", tokens.passthroughOwners);
        }

        // What the arbitration actually resolves to, from the same functions the
        // ImGui host calls. Exclusive wins over cooperative, so both are shown:
        // a plugin holding a passthrough token while something else has taken
        // exclusive capture is a state that surprises people.
        const bool exclusive   = ShouldCaptureInputNow();
        const bool cooperative = !exclusive && ShouldPassthroughInputNow();

        ImGui::TextColored(exclusive ? kBad : (cooperative ? kWarn : kOk),
                           "Game input: %s",
                           exclusive   ? "BLOCKED (exclusive capture)"
                         : cooperative ? "shared (cooperative passthrough)"
                                       : "normal");

        int panelsRegistered = 0, panelsOpen = 0;
        UI::PluginPanelRegistry::GetPanelCounts(&panelsRegistered, &panelsOpen);

        // Panels feed the exclusive predicate independently of any token, so
        // "Game input: BLOCKED" with zero tokens is explained here and nowhere
        // else. Same for the loader's own windows.
        ImGui::Text("Panels: %d open / %d registered", panelsOpen, panelsRegistered);
        ImGui::Text("Loader windows: main=%d console=%d profiler=%d notice=%d hookfail=%d",
                    UI::ModLoaderWindow::IsOpen()     ? 1 : 0,
                    UI::ConsoleWindow::IsOpen()       ? 1 : 0,
                    UI::TickProfilerWindow::IsOpen()  ? 1 : 0,
                    UI::UpdateNoticeWindow::IsOpen()  ? 1 : 0,
                    UI::HookFailureWindow::IsOpen()   ? 1 : 0);

        // ImGui's own per-frame arbitration. In cooperative mode these are what
        // decide, message by message, whether the game sees an input at all --
        // so a plugin whose widget is quietly hovered explains a game that only
        // half responds.
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("ImGui wants: mouse=%d keyboard=%d text=%d",
                    io.WantCaptureMouse    ? 1 : 0,
                    io.WantCaptureKeyboard ? 1 : 0,
                    io.WantTextInput       ? 1 : 0);
        ImGui::Text("Cursor drawn by ImGui: %d", io.MouseDrawCursor ? 1 : 0);

        int simple = 0, named = 0, advanced = 0, blocking = 0;
        Hooks::Input::GetRegistrationCounts(&simple, &named, &advanced, &blocking);

        ImGui::Text("Plugins loaded: %d", PluginManager::GetLoadedPluginCount());
        ImGui::Text("Keybinds: %d simple / %d named / %d combo", simple, named, advanced);

        // Blocking entries swallow their key before UE5 ever sees it, so one left
        // set by an unloaded plugin is a key that silently stopped working. Named
        // rather than counted: "W" is a diagnosis, "1" is a guessing game.
        ImGui::TextColored(blocking > 0 ? kWarn : kOk, "Keys blocked from game: %d", blocking);
        if (blocking > 0)
        {
            char blocked[192] = {};
            Hooks::Input::GetBlockedCombos(blocked, sizeof(blocked));
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", blocked);
        }
    }

    void RenderHud()
    {
        // Draggable HUD info box -- shown in the top-left whenever any HUD
        // option is enabled, regardless of which world is currently loaded.
        bool showFPS   = UI::GlobalSettings::GetShowFPS();
        bool showWorld = UI::GlobalSettings::GetShowWorldName();
        bool showPos   = UI::GlobalSettings::GetShowPlayerPosition();
        bool showDebug = UI::GlobalSettings::GetShowDebugValues();

        if (!showFPS && !showWorld && !showPos && !showDebug)
            return;

        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.55f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse         |
            ImGuiWindowFlags_AlwaysAutoResize   |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        // Note: no NoMove / NoSavedSettings -- the box is draggable and its
        // position is persisted to modloader_imgui.ini between sessions.

        if (ImGui::Begin("Info##hud_box", nullptr, flags))
        {
            ImGuiIO& io = ImGui::GetIO();

            if (showFPS)
                ImGui::Text("FPS: %.0f", io.Framerate);

            if (showWorld)
            {
                const char* name = UI::GlobalSettings::GetWorldName();
                ImGui::Text("World: %s", (name && name[0]) ? name : "Unknown");
            }

            if (showPos)
            {
                double x, y, z;
                bool valid;
                UI::GlobalSettings::GetPlayerPosition(&x, &y, &z, &valid);
                if (valid)
                    ImGui::Text("Pos: %.0f  %.0f  %.0f", x, y, z);
                else
                    ImGui::TextDisabled("Pos: --");
            }

            if (showDebug)
                RenderDebugValues();
        }
        ImGui::End();
    }
}

#endif // MODLOADER_CLIENT_BUILD
