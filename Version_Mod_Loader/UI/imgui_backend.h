#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// ImGuiBackend
//
// D3D12 + Win32 ImGui integration for the client build.
//
// Flow:
//   Initialize()  — reads OpenKey from modloader.ini, hooks IDXGISwapChain::Present
//                   and ID3D12CommandQueue::ExecuteCommandLists, registers F2 toggle.
//   (first Present call with queue captured) — inits D3D12 resources + ImGui backends.
//   (each Present) — runs the ImGui frame: overlay, modloader window, plugin panels.
//   Shutdown()    — removes hooks, releases D3D12 resources, shuts down ImGui backends.
//
// Plugins call ImGui through the IModLoaderImGui table returned by GetImGuiAPI().
// ---------------------------------------------------------------------------

namespace UI::ImGuiBackend
{
    // Initialize: install Present hook, read OpenKey, register toggle keybind.
    // Safe to call early (immediately after engine init).  D3D12 resource
    // creation is deferred until SetRenderingReady() is called.
    void Initialize();

    // Called once the game's rendering pipeline is fully stable (i.e. from a
    // WorldBeginPlay callback).  Until this is called the Present hook passes
    // through without initialising ImGui, avoiding conflicts with Streamline
    // and UE5 viewport setup that happen in the first few seconds.
    void SetRenderingReady();

    // Shutdown: uninstall hooks, release all resources.
    void Shutdown();

    // Returns the IModLoaderImGui function table (valid after Initialize).
    IModLoaderImGui* GetImGuiAPI();
}

#endif // MODLOADER_CLIENT_BUILD
