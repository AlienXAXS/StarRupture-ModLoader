#include "pch.h"
#include "plugin_widget_registry.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "plugin_call_tracker.h"
#include "logging/logger.h"
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
        HMODULE owner;         // module the widget's renderFn lives in, for ForgetModule
        unsigned serial;       // tells a re-used node from the entry a snapshot was taken of
    };

    static std::mutex s_mutex;
    static std::list<WidgetEntry> s_widgets;  // list: insertion never invalidates existing pointers
    static unsigned s_lastSerial = 0;
    // Calls into plugin code made outside s_mutex; see plugin_call_tracker.h.
    static PluginCallTracker s_calls;

    WidgetHandle RegisterWidget(const PluginWidgetDesc* desc)
    {
        if (!desc || !desc->name || !desc->renderFn)
            return nullptr;

        // A renderFn in no module could never be purged by ForgetModule.
        HMODULE owner = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(desc->renderFn), &owner) || !owner)
            return nullptr;

        std::lock_guard<std::mutex> lock(s_mutex);
        // Prevent duplicate names
        for (auto& e : s_widgets)
            if (_stricmp(e.desc->name, desc->name) == 0)
                return nullptr;
        s_widgets.push_back({ desc, true, owner, ++s_lastSerial });
        return static_cast<WidgetHandle>(&s_widgets.back());
    }

    void UnregisterWidget(WidgetHandle handle)
    {
        if (!handle) return;
        WidgetEntry* target = static_cast<WidgetEntry*>(handle);

        std::unique_lock<std::mutex> lock(s_mutex);
        for (auto it = s_widgets.begin(); it != s_widgets.end(); ++it)
        {
            if (&(*it) == target)
            {
                s_widgets.erase(it);

                // The caller may free desc once this returns, so a render of
                // this widget still running on another thread has to finish first.
                const bool finished = s_calls.WaitForOtherThreads(lock,
                    [&](const void* key, HMODULE) { return key == target; });
                lock.unlock();
                if (!finished)
                    ModLoaderLogger::LogError(
                        L"[PluginWidgets] UnregisterWidget: the widget's render was still running "
                        L"after %lu ms; returning anyway.", PluginCallTracker::kWaitTimeoutMs);
                return;
            }
        }
        // Handle not found — caller passed a stale or invalid handle; ignore silently.
    }

    bool ForgetModule(HMODULE module)
    {
        if (!module) return true;

        std::unique_lock<std::mutex> lock(s_mutex);
        const auto forget = [&]
        {
            s_widgets.remove_if([&](const WidgetEntry& e) { return e.owner == module; });
        };
        forget();

        // Nothing new can start in the module now. Wait out a render that
        // already has: the module is unmapped as soon as this returns. Forget
        // again after, under the same lock: a render still running in the
        // module during the wait may have registered another widget.
        const bool finished = s_calls.WaitForOtherThreads(lock,
            [&](const void*, HMODULE owner) { return owner == module; });
        forget();
        lock.unlock();
        if (!finished)
            ModLoaderLogger::LogError(
                L"[PluginWidgets] A widget render in module %p was still running after %lu ms.",
                static_cast<void*>(module), PluginCallTracker::kWaitTimeoutMs);
        return finished;
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
        // Snapshot which widgets to draw. The copy alone is not enough: the
        // desc it points to is the plugin's, gone once the plugin unregisters
        // or unloads, so each one is looked up again before it is read.
        struct WidgetRef
        {
            WidgetEntry* entry;
            unsigned     serial;
        };
        std::vector<WidgetRef> toRender;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto& e : s_widgets)
                if (e.isVisible) toRender.push_back({ &e, e.serial });
        }

        constexpr ImGuiWindowFlags kWidgetFlagsBase =
            ImGuiWindowFlags_NoCollapse         |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;

        for (const WidgetRef& ref : toRender)
        {
            std::unique_lock<std::mutex> lock(s_mutex);
            const WidgetEntry* live = FindEntry(static_cast<WidgetHandle>(ref.entry));
            if (!live || live->serial != ref.serial || !live->isVisible)
                continue;
            const WidgetEntry entry = *live;

            // Called without the lock, so the widget can call back into this
            // registry; tracked so an unload waits for it. Relocks on scope exit.
            PluginCallTracker::Scope call(s_calls, lock, ref.entry, entry.owner);

            ImGuiWindowFlags flags = kWidgetFlagsBase;
            const PluginWindowHints* hints = entry.desc->windowHints;
            if (hints)
            {
                if (hints->width > 0.0f || hints->height > 0.0f)
                    ImGui::SetNextWindowSize(ImVec2(hints->width, hints->height), (ImGuiCond)hints->size_cond);
                if (hints->pos_x >= 0.0f && hints->pos_y >= 0.0f)
                    ImGui::SetNextWindowPos(ImVec2(hints->pos_x, hints->pos_y), (ImGuiCond)hints->pos_cond, ImVec2(hints->pivot_x, hints->pivot_y));
                flags |= (ImGuiWindowFlags)hints->extra_window_flags;
            }
            else
            {
                flags |= ImGuiWindowFlags_AlwaysAutoResize;
            }

            if (ImGui::Begin(entry.desc->name, nullptr, flags))
            {
                entry.desc->renderFn(imgui);
            }
            ImGui::End();
        }
    }
}

#endif // MODLOADER_CLIENT_BUILD
