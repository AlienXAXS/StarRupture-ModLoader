#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui_host_interface.h"

// ---------------------------------------------------------------------------
// ImGuiBackend
//
// Thin loader that loads StarRupture-ImGui.dll and delegates all calls into it.
//
// Flow:
//   Initialize(cbs) — loads StarRupture-ImGui.dll, passes callbacks, installs hooks.
//   (first Present call with queue captured) — inits D3D12 resources + ImGui backends.
//   (each Present) — runs the ImGui frame via cbs.RenderFrame callback.
//   Shutdown()      — unloads the host DLL, releases D3D12 resources.
//
// The caller (client_ui.cpp) builds the ImGuiRenderCallbacks struct to wire
// the host DLL back into the main DLL's UI/input subsystems.
// ---------------------------------------------------------------------------

namespace UI::ImGuiBackend
{
    // Load StarRupture-ImGui.dll, pass callbacks, and install D3D12 Present hooks.
    // Call from InitClientUI() after loading settings and registering keybinds.
    void Initialize(const ImGuiRenderCallbacks& cbs);

    // Called once the game's rendering pipeline is fully stable (i.e. from a
    // WorldBeginPlay callback).  Until this is called the Present hook passes
    // through without initialising ImGui, avoiding conflicts with Streamline
    // and UE5 viewport setup that happen in the first few seconds.
    void SetRenderingReady();

    // Shutdown: uninstall hooks, release all resources.
    void Shutdown();

    // Returns the IModLoaderImGui function table (valid after Initialize).
    IModLoaderImGui* GetImGuiAPI();

    // Returns the IPluginImGuiTextures function table (valid after Initialize).
    IPluginImGuiTextures* GetTextureAPI();

    // Request a font atlas rebuild on the next rendered frame.
    // Safe to call from any thread.  No-op before D3D12 resources are initialised.
    void RequestFontRebuild();
}

#endif // MODLOADER_CLIENT_BUILD
