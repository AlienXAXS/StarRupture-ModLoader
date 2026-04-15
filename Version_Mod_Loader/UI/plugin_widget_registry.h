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

    // Remove a widget using the handle returned by RegisterWidget.
    void UnregisterWidget(WidgetHandle handle);

    // Show or hide a widget window.  Widgets are visible by default after registration.
    void SetWidgetVisible(WidgetHandle handle, bool visible);

    // Renders all visible widget windows.  Call once per frame at the top level
    // (outside any other ImGui window).
    void RenderWidgets(IModLoaderImGui* imgui);
}

#endif // MODLOADER_CLIENT_BUILD
