#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <string>

// ---------------------------------------------------------------------------
// Toast
//
// Short notices in the bottom-right corner of the screen, e.g. "BetterDrone
// crashed (tick callback) and was restarted". Each one fades out about seven
// seconds after it first shows; several stack upwards, newest at the bottom.
// They take no input and never capture the mouse.
//
// Show() is thread-safe (the crash restart pump calls it on the game thread);
// Render() runs inside the shared ImGui frame on the render thread.
// ---------------------------------------------------------------------------
namespace UI::Toast
{
    // Shows `text`. A toast still on screen with the same `key` (a plugin name,
    // say) is replaced and its timer starts over, so one plugin's crash and its
    // restart read as one notice that updates.
    void Show(const std::string& key, const std::string& text);

    // Draws the live toasts. Call inside an active ImGui frame.
    void Render();
}

#endif // MODLOADER_CLIENT_BUILD
