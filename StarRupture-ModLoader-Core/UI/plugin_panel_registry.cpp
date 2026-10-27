#include "pch.h"
#include "plugin_panel_registry.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "theme.h"
#include "plugin_call_tracker.h"
#include "plugins/plugin_manager.h"
#include "logging/logger.h"
#include <mutex>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace UI::PluginPanelRegistry
{
    struct PanelEntry
    {
        const PluginPanelDesc* desc;
        bool isOpen;
        char pluginName[64];   // set at registration time via SetCurrentRegistrationPlugin
        HMODULE owner;         // module the panel's renderFn lives in, for ForgetModule
        unsigned serial;       // tells a re-used node from the entry a snapshot was taken of
    };

    // Config-change callbacks are tagged with the registering plugin's stable
    // IPluginSelf* identity (same pointer passed to PluginInit -- see
    // PluginManager::LoadedPlugin::self) so FireConfigChanged can notify only
    // the plugin whose config actually changed, via pointer comparison rather
    // than a plugin-supplied name string.
    struct ConfigCallbackEntry
    {
        PluginConfigChangedCallback callback;
        const IPluginSelf* self;
    };

    struct PanelClosedEntry
    {
        PluginPanelClosedCallback callback;
        HMODULE owner;         // module the callback lives in, for ForgetModule
    };

    static std::mutex s_mutex;
    static std::list<PanelEntry> s_panels;   // list: insertion never invalidates existing pointers
    static std::vector<ConfigCallbackEntry> s_configCallbacks;
    static std::vector<PanelClosedEntry> s_panelClosedCallbacks;
    // Token -> the module that acquired it (see ResolveCallerModule). A map
    // rather than a set purely so a leak can be attributed; the token identity
    // and lifetime rules are unchanged.
    static std::map<void*, std::string> s_captureTokens;
    static std::map<void*, std::string> s_passthroughTokens;
    static char s_currentPlugin[64];   // set by plugin_manager before each PluginInit call
    static unsigned s_lastSerial = 0;
    // Calls into plugin code made outside s_mutex; see plugin_call_tracker.h.
    static PluginCallTracker s_calls;

    // The module an address lives in, or null if it is in none.
    static HMODULE ModuleOf(const void* address)
    {
        HMODULE owner = nullptr;
        if (!address ||
            !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(address), &owner))
            return nullptr;
        return owner;
    }

    // Invokes all registered panel-closed callbacks for the given handle.
    // Must NOT be called while holding s_mutex.
    static void FirePanelClosed(PanelHandle handle)
    {
        std::unique_lock<std::mutex> lock(s_mutex);
        const std::vector<PanelClosedEntry> callbacks = s_panelClosedCallbacks;
        for (const PanelClosedEntry& cb : callbacks)
        {
            // Each one is called without the lock, so an earlier one may have
            // unregistered it, or an unload dropped it and freed its module.
            const bool stillRegistered = std::any_of(
                s_panelClosedCallbacks.begin(), s_panelClosedCallbacks.end(),
                [&](const PanelClosedEntry& e) { return e.callback == cb.callback && e.owner == cb.owner; });
            if (!stillRegistered)
                continue;

            PluginCallTracker::Scope call(s_calls, lock, reinterpret_cast<const void*>(cb.callback), cb.owner);
            cb.callback(handle);
        }
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

        // Resolved before taking the lock, like the callback below. A renderFn
        // in no module could never be purged by ForgetModule, so refuse it.
        const HMODULE owner = ModuleOf(reinterpret_cast<const void*>(desc->renderFn));
        if (!owner)
            return nullptr;

        std::lock_guard<std::mutex> lock(s_mutex);
        // Prevent duplicate titles
        for (auto& e : s_panels)
            if (_stricmp(e.desc->windowTitle, desc->windowTitle) == 0)
                return nullptr;
        PanelEntry entry = {};
        entry.desc   = desc;
        entry.isOpen = false;
        entry.owner  = owner;
        entry.serial = ++s_lastSerial;
        strncpy_s(entry.pluginName, s_currentPlugin, _TRUNCATE);
        s_panels.push_back(entry);
        return static_cast<PanelHandle>(&s_panels.back());
    }

    void UnregisterPanel(PanelHandle handle)
    {
        if (!handle) return;
        PanelEntry* target = static_cast<PanelEntry*>(handle);

        std::unique_lock<std::mutex> lock(s_mutex);
        for (auto it = s_panels.begin(); it != s_panels.end(); ++it)
        {
            if (&(*it) == target)
            {
                s_panels.erase(it);

                // The caller may free desc once this returns, so a render of
                // this panel still running on another thread has to finish first.
                const bool finished = s_calls.WaitForOtherThreads(lock,
                    [&](const void* key, HMODULE) { return key == target; });
                lock.unlock();
                if (!finished)
                    ModLoaderLogger::LogError(
                        L"[PluginPanels] UnregisterPanel: the panel's render was still running "
                        L"after %lu ms; returning anyway.", PluginCallTracker::kWaitTimeoutMs);
                return;
            }
        }
        // Handle not found — caller passed a stale or invalid handle; ignore silently.
    }

    void RegisterOnConfigChanged(const IPluginSelf* self, PluginConfigChangedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        ConfigCallbackEntry entry = {};
        entry.callback = callback;
        entry.self     = self;
        s_configCallbacks.push_back(entry);
    }

    void UnregisterOnConfigChanged(const IPluginSelf* self, PluginConfigChangedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_configCallbacks.erase(
            std::remove_if(s_configCallbacks.begin(), s_configCallbacks.end(),
                           [&](const ConfigCallbackEntry& e) { return e.callback == callback && e.self == self; }),
            s_configCallbacks.end());
    }

    void ForgetConfigCallbacks(const IPluginSelf* self)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_configCallbacks.erase(
            std::remove_if(s_configCallbacks.begin(), s_configCallbacks.end(),
                           [&](const ConfigCallbackEntry& e) { return e.self == self; }),
            s_configCallbacks.end());
    }

    void FireConfigChanged(const char* pluginName, const char* section, const char* key, const char* newValue)
    {
        // Resolve the changed config's owning plugin to its stable self pointer
        // once, then filter callbacks by pointer identity instead of a string
        // compare -- also sidesteps any case-sensitivity mismatch entirely.
        const IPluginSelf* changedSelf = pluginName ? PluginManager::GetSelfForPlugin(pluginName) : nullptr;

        std::vector<PluginConfigChangedCallback> toCall;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto& e : s_configCallbacks)
                if (changedSelf && e.self == changedSelf)
                    toCall.push_back(e.callback);
        }
        for (auto cb : toCall)
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
        const HMODULE owner = ModuleOf(reinterpret_cast<const void*>(callback));
        if (!owner) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_panelClosedCallbacks.push_back({ callback, owner });
    }

    void UnregisterOnPanelWindowClosed(PluginPanelClosedCallback callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_panelClosedCallbacks.erase(
            std::remove_if(s_panelClosedCallbacks.begin(), s_panelClosedCallbacks.end(),
                           [&](const PanelClosedEntry& e) { return e.callback == callback; }),
            s_panelClosedCallbacks.end());
    }

    bool ForgetModule(HMODULE module)
    {
        if (!module) return true;

        std::unique_lock<std::mutex> lock(s_mutex);
        const auto forget = [&]
        {
            s_panels.remove_if([&](const PanelEntry& e) { return e.owner == module; });
            s_panelClosedCallbacks.erase(
                std::remove_if(s_panelClosedCallbacks.begin(), s_panelClosedCallbacks.end(),
                               [&](const PanelClosedEntry& e) { return e.owner == module; }),
                s_panelClosedCallbacks.end());
        };
        forget();

        // Nothing new can start in the module now. Wait out what already has,
        // including the render of a panel the plugin unregistered itself: the
        // module is unmapped as soon as this returns. Forget again after, under
        // the same lock: a call still running in the module during the wait
        // may have registered another panel or callback.
        const bool finished = s_calls.WaitForOtherThreads(lock,
            [&](const void*, HMODULE owner) { return owner == module; });
        forget();
        lock.unlock();
        if (!finished)
            ModLoaderLogger::LogError(
                L"[PluginPanels] A panel render or panel-closed callback in module %p was still "
                L"running after %lu ms.", static_cast<void*>(module), PluginCallTracker::kWaitTimeoutMs);
        return finished;
    }

    // -----------------------------------------------------------------------
    // Input tokens
    //
    // A leaked token is the worst bug this file can have: while one is held the
    // game is either cut out of input entirely or sharing it with ImGui, and
    // nothing on screen says so. It is also the hardest to attribute, because by
    // the time anyone notices, the plugin that took it may have been unloaded.
    //
    // So each token records which module asked for it. The plugin never tells us
    // -- the acquire functions carry no plugin context and adding one would mean
    // an interface break for every existing plugin -- so it is read off the call
    // stack instead: walk outwards until a frame belongs to a module that is not
    // this one, and that is the caller's DLL. Diagnostic only; a failure to
    // resolve records "unknown" and changes nothing else.
    // -----------------------------------------------------------------------

    // The module this function lives in, i.e. the modloader core. Cached: it
    // cannot change for the life of the process.
    static HMODULE SelfModule()
    {
        static HMODULE s_self = []
        {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&SelfModule), &self);
            return self;
        }();
        return s_self;
    }

    static std::string ResolveCallerModule()
    {
        // 16 frames is generous: the real caller is two or three out (plugin ->
        // hooks_interface wrapper -> here), and anything deeper than this is not
        // a stack we would be able to interpret anyway.
        void* frames[16] = {};
        const USHORT captured = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
        const HMODULE self = SelfModule();

        for (USHORT i = 0; i < captured; ++i)
        {
            HMODULE owner = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(frames[i]), &owner))
                continue;

            if (!owner || owner == self)
                continue;   // still inside the modloader -- keep walking out

            char path[MAX_PATH] = {};
            if (GetModuleFileNameA(owner, path, MAX_PATH) == 0)
                continue;

            const char* leaf = std::strrchr(path, '\\');
            return leaf ? leaf + 1 : path;
        }

        return "unknown";
    }

    // Collapses the owner list into "CameraControls.dll x2, Other.dll" -- one
    // entry per module, so a plugin that leaks a token every time it opens its
    // editor reads as a growing count against a single name rather than as an
    // unreadable wall of identical lines.
    static void SummariseOwners(const std::map<void*, std::string>& tokens,
                                char* out, int outSize)
    {
        if (outSize <= 0) return;
        out[0] = '\0';

        std::vector<std::pair<std::string, int>> counts;
        for (const auto& entry : tokens)
        {
            auto it = std::find_if(counts.begin(), counts.end(),
                                   [&](const std::pair<std::string, int>& c)
                                   { return c.first == entry.second; });
            if (it != counts.end())
                ++it->second;
            else
                counts.emplace_back(entry.second, 1);
        }

        // snprintf, not _snprintf_s with _TRUNCATE: the latter returns -1 when it
        // truncates, and adding that to the cursor walks it backwards and corrupts
        // the buffer. snprintf returns the length it *wanted*, so overrun is
        // detected by comparing against the space that was left.
        int used = 0;
        for (const auto& count : counts)
        {
            const int remaining = outSize - used;
            if (remaining <= 1) break;

            const int written = count.second > 1
                ? snprintf(out + used, remaining, "%s%s x%d",
                           used > 0 ? ", " : "", count.first.c_str(), count.second)
                : snprintf(out + used, remaining, "%s%s",
                           used > 0 ? ", " : "", count.first.c_str());

            if (written < 0 || written >= remaining)
                break;   // truncated; keep what fitted rather than lose the lot

            used += written;
        }
    }

    void* AcquireInputCapture()
    {
        void* token = new char;

        // Resolved before taking the lock: walking the stack touches the loader
        // lock, and holding two locks in an order nobody else observes is how a
        // deadlock gets built.
        std::string owner = ResolveCallerModule();

        std::lock_guard<std::mutex> lock(s_mutex);
        s_captureTokens.emplace(token, std::move(owner));
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

    void* AcquireInputPassthrough()
    {
        void* token = new char;
        std::string owner = ResolveCallerModule();

        std::lock_guard<std::mutex> lock(s_mutex);
        s_passthroughTokens.emplace(token, std::move(owner));
        return token;
    }

    void ReleaseInputPassthrough(void* token)
    {
        if (!token) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_passthroughTokens.find(token);
        if (it == s_passthroughTokens.end())
            return; // unknown or already-released token -- ignore
        s_passthroughTokens.erase(it);
        delete static_cast<char*>(token);
    }

    bool AnyInputPassthroughRequested()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return !s_passthroughTokens.empty();
    }

    void GetInputTokenSummary(InputTokenSummary* out)
    {
        if (!out) return;
        *out = InputTokenSummary{};

        std::lock_guard<std::mutex> lock(s_mutex);
        out->captureCount     = static_cast<int>(s_captureTokens.size());
        out->passthroughCount = static_cast<int>(s_passthroughTokens.size());
        SummariseOwners(s_captureTokens,     out->captureOwners,     sizeof(out->captureOwners));
        SummariseOwners(s_passthroughTokens, out->passthroughOwners, sizeof(out->passthroughOwners));
    }

    void GetPanelCounts(int* outRegistered, int* outOpen)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (outRegistered) *outRegistered = static_cast<int>(s_panels.size());
        if (outOpen)
        {
            int open = 0;
            for (const auto& e : s_panels)
                if (e.isOpen) ++open;
            *outOpen = open;
        }
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
        // Snapshot which panels to draw, not pointers to use: an entry can be
        // erased (an unload, or UnregisterPanel on any thread) whenever the
        // lock is not held, so each one is looked up again before it is read.
        struct PanelRef
        {
            PanelEntry* entry;
            unsigned    serial;
        };
        std::vector<PanelRef> toRender;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto& e : s_panels)
                if (e.isOpen) toRender.push_back({ &e, e.serial });
        }

        // Must be called with s_mutex held.
        const auto find = [](const PanelRef& ref) -> PanelEntry*
        {
            PanelEntry* e = FindEntry(static_cast<PanelHandle>(ref.entry));
            return e && e->serial == ref.serial ? e : nullptr;
        };

        for (const PanelRef& ref : toRender)
        {
            std::unique_lock<std::mutex> lock(s_mutex);
            const PanelEntry* live = find(ref);
            if (!live || !live->isOpen)
                continue;
            const PanelEntry panel = *live;

            bool open = true;
            {
                // Not held across renderFn: a plugin's own render callback is
                // free to call back into this registry (SetPanelClose on
                // itself, RegisterPanel, etc.), and locking here would either
                // deadlock that or block whatever else is waiting on s_mutex
                // for the length of a plugin's render. The call is tracked
                // instead, so an unload waits for it before the module goes.
                PluginCallTracker::Scope call(s_calls, lock, ref.entry, panel.owner);

                ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);

                // Subtitle shows which plugin owns the panel -- nullptr (omitted)
                // for panels registered without a recorded owner.
                const char* subtitle = panel.pluginName[0] ? panel.pluginName : nullptr;
                if (UI::Theme::BeginChamferedWindow(panel.desc->windowTitle, panel.desc->windowTitle,
                                                     &open, subtitle))
                {
                    panel.desc->renderFn(imgui);
                    UI::Theme::EndChamferedWindow();
                }
            }
            // s_mutex is held again here.

            // Only write back a close made by ImGui itself (the titlebar X).
            // `open` was snapshotted before renderFn ran, so assigning it
            // unconditionally undid any SetPanelClose the plugin issued from
            // inside its own render callback (e.g. a "Close" button) or from
            // another thread meanwhile: the panel stayed open even though the
            // plugin had already been told, via FirePanelClosed, that it shut.
            if (!open)
            {
                bool wasOpen = false;
                if (PanelEntry* e = find(ref))
                {
                    wasOpen = e->isOpen;
                    e->isOpen = false;
                }
                lock.unlock();
                // A SetPanelClose during render has already fired the callback.
                if (wasOpen)
                    FirePanelClosed(static_cast<PanelHandle>(ref.entry));
            }
        }
    }
}

#endif // MODLOADER_CLIENT_BUILD
