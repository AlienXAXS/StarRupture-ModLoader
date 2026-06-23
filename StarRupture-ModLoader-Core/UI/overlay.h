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

    // Show or hide the modloader watermark bar (bottom-right corner).
    // Pass true when the main menu world is active, false for all other worlds.
    // Does not affect the HUD info box -- that is controlled by GlobalSettings toggles.
    void SetVisible(bool visible);

    // Render this frame's watermark bar.  Call inside an active ImGui frame.
    void Render();

    // Render the always-on HUD info box (FPS, world name, player position) as
    // a small draggable window in the top-left corner.  Shown whenever any HUD
    // option is enabled, regardless of which world is loaded.
    void RenderHud();
}

#endif // MODLOADER_CLIENT_BUILD
