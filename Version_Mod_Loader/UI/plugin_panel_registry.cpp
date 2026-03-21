#include "pch.h"
#include "plugin_panel_registry.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

namespace UI::PluginPanelRegistry
{
    struct PanelEntry
    {
        const PluginPanelDesc* desc;
        bool isOpen;
    };

    static std::mutex s_mutex;
    static std::vector<PanelEntry> s_panels;
    static std::vector<PluginConfigChangedCallback> s_configCallbacks;

    void RegisterPanel(const PluginPanelDesc* desc)
    {
        if (!desc || !desc->windowTitle || !desc->renderFn)
            return;

        std::lock_guard<std::mutex> lock(s_mutex);
        // Prevent duplicate titles
        for (auto& e : s_panels)
            if (_stricmp(e.desc->windowTitle, desc->windowTitle) == 0)
                return;
        s_panels.push_back({ desc, false });
    }

    void UnregisterPanel(const char* windowTitle)
    {
        if (!windowTitle) return;

        std::lock_guard<std::mutex> lock(s_mutex);
        s_panels.erase(
            std::remove_if(s_panels.begin(), s_panels.end(),
                [&](const PanelEntry& e) {
                    return _stricmp(e.desc->windowTitle, windowTitle) == 0;
                }),
            s_panels.end());
    }

    void RegisterOnConfigChanged(PluginConfigChangedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_configCallbacks.push_back(callback);
    }

    void UnregisterOnConfigChanged(PluginConfigChangedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_configCallbacks.erase(
            std::remove(s_configCallbacks.begin(), s_configCallbacks.end(), callback),
            s_configCallbacks.end());
    }

    void FireConfigChanged(const char* section, const char* key, const char* newValue)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto cb : s_configCallbacks)
            cb(section, key, newValue);
    }

    void RenderPanelButtons(IModLoaderImGui* imgui)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& entry : s_panels)
        {
            const char* label = entry.desc->buttonLabel ? entry.desc->buttonLabel : entry.desc->windowTitle;
            if (imgui->Button(label))
                entry.isOpen = true;
        }
    }

    void RenderPanelWindows(IModLoaderImGui* imgui)
    {
        // Snapshot to avoid holding lock during render callbacks
        std::vector<PanelEntry*> toRender;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto& e : s_panels)
                if (e.isOpen) toRender.push_back(&e);
        }

        for (PanelEntry* entry : toRender)
        {
            ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
            bool open = entry->isOpen;
            if (ImGui::Begin(entry->desc->windowTitle, &open))
            {
                entry->desc->renderFn(imgui);
            }
            ImGui::End();
            entry->isOpen = open;
        }
    }
}

#endif // MODLOADER_CLIENT_BUILD
