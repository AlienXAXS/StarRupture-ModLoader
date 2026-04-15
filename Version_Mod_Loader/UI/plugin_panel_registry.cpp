#include "pch.h"
#include "plugin_panel_registry.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include <mutex>
#include <list>
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
    static std::list<PanelEntry> s_panels;   // list: insertion never invalidates existing pointers
    static std::vector<PluginConfigChangedCallback> s_configCallbacks;

    PanelHandle RegisterPanel(const PluginPanelDesc* desc)
    {
        if (!desc || !desc->windowTitle || !desc->renderFn)
            return nullptr;

        std::lock_guard<std::mutex> lock(s_mutex);
        // Prevent duplicate titles
        for (auto& e : s_panels)
            if (_stricmp(e.desc->windowTitle, desc->windowTitle) == 0)
                return nullptr;
        s_panels.push_back({ desc, false });
        return static_cast<PanelHandle>(&s_panels.back());
    }

    void UnregisterPanel(PanelHandle handle)
    {
        if (!handle) return;
        PanelEntry* target = static_cast<PanelEntry*>(handle);

        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto it = s_panels.begin(); it != s_panels.end(); ++it)
        {
            if (&(*it) == target)
            {
                s_panels.erase(it);
                return;
            }
        }
        // Handle not found — caller passed a stale or invalid handle; ignore silently.
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

    // Returns the PanelEntry* if the handle is a known registered panel, otherwise null.
    // Must be called with s_mutex held.
    static PanelEntry* FindEntry(PanelHandle handle)
    {
        if (!handle) return nullptr;
        PanelEntry* target = static_cast<PanelEntry*>(handle);
        for (auto& e : s_panels)
            if (&e == target) return target;
        return nullptr;
    }

    void SetPanelOpen(PanelHandle handle)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (PanelEntry* e = FindEntry(handle))
            e->isOpen = true;
    }

    void SetPanelClose(PanelHandle handle)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (PanelEntry* e = FindEntry(handle))
            e->isOpen = false;
    }

    void CloseAllPanels()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& e : s_panels)
            e.isOpen = false;
    }

    bool AnyPanelOpen()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto& e : s_panels)
            if (e.isOpen) return true;
        return false;
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
