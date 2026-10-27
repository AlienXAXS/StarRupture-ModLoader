#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// PluginWidgetRegistry
//
// Thread-safe storage for plugin-registered always-on ImGui widgets.
// Plugins call hooks->UI->RegisterWidget to add a widget; the modloader
// renders each visible widget window every frame regardless of modloader
// window state.  Visibility can be toggled via SetWidgetVisible.
// ---------------------------------------------------------------------------

namespace UI::PluginWidgetRegistry
{
    // Register a widget.  desc and all strings it points to must remain valid
    // until UnregisterWidget is called.  Returns an opaque WidgetHandle
    // (internally WidgetEntry*), or null on failure.
    WidgetHandle RegisterWidget(const PluginWidgetDesc* desc);

    // Remove a widget using the handle returned by RegisterWidget. If another
    // thread is drawing the widget, waits (bounded) for that to finish, so desc
    // may be freed once this returns.
    void UnregisterWidget(WidgetHandle handle);

    // Drop every widget whose renderFn lives in the given module. For the
    // plugin manager to call just before it FreeLibrary()s a plugin: a widget
    // is drawn every frame from pointers into the plugin's image, so one the
    // plugin did not unregister itself would be called after the module is gone.
    // Then waits (bounded) for any render of the module still running on
    // another thread. Returns false if one still was when the wait gave up:
    // the module must not be freed then.
    bool ForgetModule(HMODULE module);

    // Show or hide a widget window.  Widgets are visible by default after registration.
    void SetWidgetVisible(WidgetHandle handle, bool visible);

    // Renders all visible widget windows.  Call once per frame at the top level
    // (outside any other ImGui window).
    void RenderWidgets(IModLoaderImGui* imgui);
}

#endif // MODLOADER_CLIENT_BUILD
