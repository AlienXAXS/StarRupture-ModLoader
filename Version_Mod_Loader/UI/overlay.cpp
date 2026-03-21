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
        bool showFPS    = UI::GlobalSettings::GetShowFPS();
        bool showWorld  = UI::GlobalSettings::GetShowWorldName();
        bool showPos    = UI::GlobalSettings::GetShowPlayerPosition();
        bool anyHud     = showFPS || showWorld || showPos;

        if (!s_visible && !anyHud)
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
            // Build one line: [FPS | ] [World | ] [x y z | ] ModLoader ... | F2
            char buf[256];
            int  pos = 0;

            if (showFPS)
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%.0f FPS | ", io.Framerate);

            if (showWorld)
            {
                const char* name = UI::GlobalSettings::GetWorldName();
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%s | ", (name && name[0]) ? name : "Unknown");
            }

            if (showPos)
            {
                double x, y, z;
                bool valid;
                UI::GlobalSettings::GetPlayerPosition(&x, &y, &z, &valid);
                if (valid)
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                                    "%.0f %.0f %.0f | ", x, y, z);
                else
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "-- | ");
            }

            int pluginCount = ModLoaderLogger::GetLoadedPluginCount();
            snprintf(buf + pos, sizeof(buf) - pos,
                     "AlienX's Mod Loader %s | %d Plugin%s Loaded | %s To Open UI",
                     MODLOADER_BUILD_TAG,
                     pluginCount,
                     pluginCount == 1 ? "" : "s",
                     s_openKeyName);

            ImGui::TextUnformatted(buf);
        }
        ImGui::End();
    }

    void RenderHud()
    {
        // HUD items are now part of Render() -- nothing to do here.
    }
}

#endif // MODLOADER_CLIENT_BUILD
