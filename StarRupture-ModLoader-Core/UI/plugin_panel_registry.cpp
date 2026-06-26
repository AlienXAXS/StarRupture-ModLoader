#include "pch.h"
#include "plugin_panel_registry.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include <mutex>
#include <list>
#include <vector>
#include <set>
#include <algorithm>
#include <cstring>

namespace UI::PluginPanelRegistry
{
    struct PanelEntry
    {
        const PluginPanelDesc* desc;
        bool isOpen;
        char pluginName[64];   // set at registration time via SetCurrentRegistrationPlugin
    };

    static std::mutex s_mutex;
    static std::list<PanelEntry> s_panels;   // list: insertion never invalidates existing pointers
    static std::vector<PluginConfigChangedCallback> s_configCallbacks;
    static std::vector<PluginPanelClosedCallback> s_panelClosedCallbacks;
    static std::set<void*> s_captureTokens;
    static char s_currentPlugin[64];   // set by plugin_manager before each PluginInit call

    // Invokes all registered panel-closed callbacks for the given handle.
    // Must NOT be called while holding s_mutex.
    static void FirePanelClosed(PanelHandle handle)
    {
        std::vector<PluginPanelClosedCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            callbacks = s_panelClosedCallbacks;
        }
        for (auto cb : callbacks)
            cb(handle);
    }

    void SetCurrentRegistrationPlugin(const char* name)
    {
        if (name)
            strncpy_s(s_currentPlugin, name, _TRUNCATE);
        else
            s_currentPlugin[0] = '\0';
    }

    PanelHandle RegisterPanel(const PluginPanelDesc* desc)
    {
        if (!desc || !desc->windowTitle || !desc->renderFn)
            return nullptr;

        std::lock_guard<std::mutex> lock(s_mutex);
        // Prevent duplicate titles
        for (auto& e : s_panels)
            if (_stricmp(e.desc->windowTitle, desc->windowTitle) == 0)
                return nullptr;
        PanelEntry entry = {};
        entry.desc   = desc;
        entry.isOpen = false;
        strncpy_s(entry.pluginName, s_currentPlugin, _TRUNCATE);
        s_panels.push_back(entry);
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
        bool wasOpen = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (PanelEntry* e = FindEntry(handle))
            {
                wasOpen = e->isOpen;
                e->isOpen = false;
            }
        }
        if (wasOpen)
            FirePanelClosed(handle);
    }

    void RegisterOnPanelWindowClosed(PluginPanelClosedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_panelClosedCallbacks.push_back(callback);
    }

    void UnregisterOnPanelWindowClosed(PluginPanelClosedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_panelClosedCallbacks.erase(
            std::remove(s_panelClosedCallbacks.begin(), s_panelClosedCallbacks.end(), callback),
            s_panelClosedCallbacks.end());
    }

    void* AcquireInputCapture()
    {
        void* token = new char;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_captureTokens.insert(token);
        return token;
    }

    void ReleaseInputCapture(void* token)
    {
        if (!token) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_captureTokens.find(token);
        if (it == s_captureTokens.end())
            return; // unknown or already-released token -- ignore
        s_captureTokens.erase(it);
        delete static_cast<char*>(token);
    }

    bool AnyInputCaptureRequested()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return !s_captureTokens.empty();
    }

    bool AnyPanelOpen()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto& e : s_panels)
            if (e.isOpen) return true;
        return false;
    }

    void RenderPanelButtons(IModLoaderImGui* imgui, const char* pluginName)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& entry : s_panels)
        {
            // Skip if this panel belongs to a different plugin.
            // Panels with no recorded owner (empty pluginName) show for all plugins.
            if (pluginName && entry.pluginName[0] != '\0' &&
                _stricmp(entry.pluginName, pluginName) != 0)
                continue;
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
            // Subtitle shows which plugin owns the panel -- nullptr (omitted)
            // for panels registered without a recorded owner.
            const char* subtitle = entry->pluginName[0] ? entry->pluginName : nullptr;
            if (UI::Theme::BeginChamferedWindow(entry->desc->windowTitle, entry->desc->windowTitle,
                                                 &open, subtitle))
            {
                entry->desc->renderFn(imgui);
                UI::Theme::EndChamferedWindow();
            }
            entry->isOpen = open;
            if (!open)
                FirePanelClosed(static_cast<PanelHandle>(entry));
        }
    }
}

#endif // MODLOADER_CLIENT_BUILD
