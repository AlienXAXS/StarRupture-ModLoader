#include "pch.h"
#include "plugin_widget_registry.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include <mutex>
#include <list>
#include <vector>
#include <cstring>

namespace UI::PluginWidgetRegistry
{
    struct WidgetEntry
    {
        const PluginWidgetDesc* desc;
        bool isVisible;
    };

    static std::mutex s_mutex;
    static std::list<WidgetEntry> s_widgets;  // list: insertion never invalidates existing pointers

    WidgetHandle RegisterWidget(const PluginWidgetDesc* desc)
    {
        if (!desc || !desc->name || !desc->renderFn)
            return nullptr;

        std::lock_guard<std::mutex> lock(s_mutex);
        // Prevent duplicate names
        for (auto& e : s_widgets)
            if (_stricmp(e.desc->name, desc->name) == 0)
                return nullptr;
        s_widgets.push_back({ desc, true });
        return static_cast<WidgetHandle>(&s_widgets.back());
    }

    void UnregisterWidget(WidgetHandle handle)
    {
        if (!handle) return;
        WidgetEntry* target = static_cast<WidgetEntry*>(handle);

        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto it = s_widgets.begin(); it != s_widgets.end(); ++it)
        {
            if (&(*it) == target)
            {
                s_widgets.erase(it);
                return;
            }
        }
        // Handle not found — caller passed a stale or invalid handle; ignore silently.
    }

    // Returns the WidgetEntry* if the handle is a known registered widget, otherwise null.
    // Must be called with s_mutex held.
    static WidgetEntry* FindEntry(WidgetHandle handle)
    {
        if (!handle) return nullptr;
        WidgetEntry* target = static_cast<WidgetEntry*>(handle);
        for (auto& e : s_widgets)
            if (&e == target) return target;
        return nullptr;
    }

    void SetWidgetVisible(WidgetHandle handle, bool visible)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (WidgetEntry* e = FindEntry(handle))
            e->isVisible = visible;
    }

    void RenderWidgets(IModLoaderImGui* imgui)
    {
        // Snapshot visible entries to avoid holding lock during render callbacks
        std::vector<WidgetEntry*> toRender;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto& e : s_widgets)
                if (e.isVisible) toRender.push_back(&e);
        }

        constexpr ImGuiWindowFlags kWidgetFlags =
            ImGuiWindowFlags_NoCollapse       |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;

        for (WidgetEntry* entry : toRender)
        {
            if (ImGui::Begin(entry->desc->name, nullptr, kWidgetFlags))
            {
                entry->desc->renderFn(imgui);
            }
            ImGui::End();
        }
    }
}

#endif // MODLOADER_CLIENT_BUILD
