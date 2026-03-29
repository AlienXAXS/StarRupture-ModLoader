#pragma once

#include "plugin_interface.h"

// Owns the PanelHandle returned by hooks->UI->RegisterPanel.
// Drives all ImGui rendering for the SRCheats window.
// Must only be Init'd when hooks->UI != nullptr (client builds only).
class CheatsPanel
{
public:
    static void Init(IPluginHooks* hooks);
    static void Shutdown(IPluginHooks* hooks);

    // ImGui render callback - must be public so it can be stored in PluginPanelDesc at file scope
    static void OnRender(IModLoaderImGui* imgui);

private:
    static PanelHandle        s_panelHandle;
    static IPluginUIEvents*   s_ui;      // cached for use inside static keybind callback
    static IPluginInputEvents* s_input;  // cached so Shutdown can unregister the keybind

    // F5 keybind callback - toggles the cheat panel open/closed
    static void OnHotkey(EModKey key, EModKeyEvent event);

    // Section renderers
    static void RenderPlayerSection(IModLoaderImGui* imgui);
    static void RenderWorldSection(IModLoaderImGui* imgui);
    static void RenderSurvivalSection(IModLoaderImGui* imgui);

    // Persistent UI state for text/int inputs
    static char s_itemNameBuf[256];
    static int  s_itemAmount;
    static char s_attrNameBuf[64];
    static int  s_attrValue;
};
