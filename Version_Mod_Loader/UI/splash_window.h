#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// ---------------------------------------------------------------------------
// Splash Window -- client-only startup progress overlay
//
// Shows a small always-on-top window during mod loader initialization.
// Only active when MODLOADER_CLIENT_BUILD is defined; all calls are
// no-ops on server builds.
// ---------------------------------------------------------------------------

namespace Splash
{
    // Create and show the splash window on a background thread.
  // Safe to call from DllMain (DLL_PROCESS_ATTACH).
    void Show();

    // Update the status text shown on the splash window.
    // Thread-safe -- can be called from any thread.
    void SetStatus(const wchar_t* text);

    // Update the progress bar (0.0 -- 1.0).
    // Thread-safe -- can be called from any thread.
    void SetProgress(float fraction);

    // Switch the progress bar to red and fill it completely.
    // Call before displaying an error/countdown message.
    // Pass showCloseButton=false when the game will continue without mods
    // (e.g. version mismatch) so the splash closes on its own rather than
    // requiring the user to click a "close game" button.
    void SetErrorMode(bool showCloseButton = true);

    // Update the secondary status label shown below the main progress bar.
    // Pass an empty string or call ClearSubBar() to hide the secondary section.
    void SetSubStatus(const wchar_t* text);

    // Update the secondary progress bar fill (0.0 -- 1.0).
    void SetSubProgress(float fraction);

    // Hide the secondary bar and clear its label.
    void ClearSubBar();

    // Returns true while the splash window is open and visible.
    // Use this to guard plugin calls -- returns false after the splash has
    // closed (e.g. during a hot-reload after startup).
    // Thread-safe -- can be called from any thread.
    bool IsVisible();

    // Pump messages for the given duration (milliseconds) so cross-thread
    // repaint requests (WM_APP) posted during e.g. PluginInit are dispatched
    // before the splash closes.  Call this instead of Sleep() during the
    // final "hold" period.
    void Linger(DWORD ms);

    // Close the splash window and clean up the background thread.
    // Blocks briefly until the window thread exits.
    void Close();
}
