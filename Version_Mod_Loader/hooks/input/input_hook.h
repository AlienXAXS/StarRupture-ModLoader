#pragma once

// Client-only: UGameViewportClient::InputKey detour.
// Intercepts all key events before UE5 processes them, fires plugin keybind
// callbacks, and optionally consumes the input (blocking mode).

#ifdef MODLOADER_CLIENT_BUILD

namespace Hooks::InputHook
{
    // AOB scan + install the InputKey detour.
    // Called from ImGuiBackend::SetRenderingReady() once the D3D12 overlay is live.
    void Install();

    // Uninstall the detour and clear the FName cache.
    // Called from ImGuiBackend::Shutdown().
    void Remove();
}

#endif // MODLOADER_CLIENT_BUILD
