#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// PluginPanelRegistry
//
// Thread-safe storage for plugin-registered ImGui panels.
// Plugins call hooks->UI->RegisterPanel to add a panel; the modloader renders
// a button per panel and calls the plugin's renderFn each frame while open.
// ---------------------------------------------------------------------------

namespace UI::PluginPanelRegistry
{
    // Set the plugin name to associate with the next RegisterPanel call(s).
    // Call before PluginInit, clear to nullptr after.
    void SetCurrentRegistrationPlugin(const char* name);

    // Register a panel.  desc and all strings it points to must remain valid
    // until UnregisterPanel is called.  Returns an opaque PanelHandle
    // (internally PanelEntry*), or null on failure.
    PanelHandle RegisterPanel(const PluginPanelDesc* desc);

    // Remove a panel using the handle returned by RegisterPanel.
    void UnregisterPanel(PanelHandle handle);

    // Register/unregister a config-change notification callback. self is the
    // registering plugin's IPluginSelf* (same pointer passed to PluginInit) --
    // FireConfigChanged only invokes callbacks whose self matches the plugin
    // whose config actually changed.
    void RegisterOnConfigChanged(const IPluginSelf* self, PluginConfigChangedCallback callback);
    void UnregisterOnConfigChanged(const IPluginSelf* self, PluginConfigChangedCallback callback);

    // Open or close a panel using the handle returned by RegisterPanel.
    void SetPanelOpen(PanelHandle handle);
    void SetPanelClose(PanelHandle handle);

    // Register/unregister a panel-closed notification callback. Fired with
    // the handle of the panel that was closed, whether via the ImGui
    // titlebar X button (RenderPanelWindows) or via SetPanelClose.
    void RegisterOnPanelWindowClosed(PluginPanelClosedCallback callback);
    void UnregisterOnPanelWindowClosed(PluginPanelClosedCallback callback);

    // Acquire/release an input-capture request token. While at least one
    // token is held, AnyInputCaptureRequested() returns true.
    void* AcquireInputCapture();
    void  ReleaseInputCapture(void* token);

    // Returns true if any plugin currently holds an input-capture token.
    bool AnyInputCaptureRequested();

    // Acquire/release a cooperative input-passthrough token. While at least
    // one token is held (and nothing is asking for exclusive capture), ImGui
    // is fed every message and owns the cursor, but the game keeps receiving
    // input except where ImGui actually wants it.
    void* AcquireInputPassthrough();
    void  ReleaseInputPassthrough(void* token);

    // Returns true if any plugin currently holds an input-passthrough token.
    bool AnyInputPassthroughRequested();

    // Returns true if at least one plugin panel window is currently open.
    // Used by imgui_backend to decide whether to capture the mouse even when
    // the main modloader window is closed.
    bool AnyPanelOpen();

    // Called by modloader_window to fire config-change notifications.
    // pluginName is the plugin that owns the changed config file -- only
    // callbacks registered by that plugin are invoked.
    void FireConfigChanged(const char* pluginName, const char* section, const char* key, const char* newValue);

    // Renders "Open" buttons for panels belonging to pluginName (null = all).
    // Call from inside an ImGui window; handles Begin/End for each panel window.
    void RenderPanelButtons(IModLoaderImGui* imgui, const char* pluginName = nullptr);

    // Renders all open panel windows.  Call once per frame at the top level
    // (outside any other ImGui window).
    void RenderPanelWindows(IModLoaderImGui* imgui);
}

#endif // MODLOADER_CLIENT_BUILD
