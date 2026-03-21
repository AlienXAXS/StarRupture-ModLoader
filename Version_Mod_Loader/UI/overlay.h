#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Overlay
//
// Small always-on corner watermark showing modloader status.
// Rendered every frame before ImGui::Render().
// ---------------------------------------------------------------------------

namespace UI::Overlay
{
    // Set the open-key name string shown in the hint (e.g. "F2").
    void SetOpenKeyName(const char* keyName);

    // Show or hide the overlay.  Pass true when the main menu world is active,
    // false for all other worlds.
    void SetVisible(bool visible);

    // Render this frame's overlay.  Call inside an active ImGui frame.
    void Render();

    // Render the always-on HUD overlay (FPS, world name, player position).
    // Shown regardless of whether the modloader watermark is visible.
    void RenderHud();
}

#endif // MODLOADER_CLIENT_BUILD
