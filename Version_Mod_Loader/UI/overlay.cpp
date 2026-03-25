#include "pch.h"
#include "overlay.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "plugins/plugin_manager.h"
#include "global_settings.h"
#include <cstdio>
#include <cstring>

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "local-build"
#endif

namespace UI::Overlay
{
    static char s_openKeyName[16] = "F2";
    static bool s_visible         = false;

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

        // Bottom-right corner, 10 px from edges
        const float PAD = 10.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - PAD, io.DisplaySize.y - PAD),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
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
            int pluginCount = ModLoaderLogger::GetLoadedPluginCount();
            ImGui::Text("AlienX's Mod Loader %s | %d Plugin%s Loaded | %s To Open UI",
                        MODLOADER_BUILD_TAG,
                        pluginCount,
                        pluginCount == 1 ? "" : "s",
                        s_openKeyName);
        }
        ImGui::End();
    }

    void RenderHud()
    {
        // Draggable HUD info box -- shown in the top-left whenever any HUD
        // option is enabled, regardless of which world is currently loaded.
        bool showFPS   = UI::GlobalSettings::GetShowFPS();
        bool showWorld = UI::GlobalSettings::GetShowWorldName();
        bool showPos   = UI::GlobalSettings::GetShowPlayerPosition();

        if (!showFPS && !showWorld && !showPos)
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
        }
        ImGui::End();
    }
}

#endif // MODLOADER_CLIENT_BUILD
