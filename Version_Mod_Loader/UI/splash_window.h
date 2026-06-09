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

    // Increment the hold counter to defer the normal Linger/Close sequence.
    // Call from PluginInit (before returning) if your plugin posts async work
    // to the game thread that needs to update the splash.  You MUST call
    // ReleaseHold exactly once when that work finishes (or fails).
    // Safe to call from any thread.  No-op if the splash is already closed.
    void AcquireHold();

    // Decrement the hold counter.  When it reaches zero the WaitForAllHolds
    // call in the init thread unblocks and the splash proceeds to close.
    // Safe to call from any thread.
    void ReleaseHold();

    // Returns true if any plugin currently holds the splash open.
    bool HasHolds();

    // Block until the hold counter reaches zero or timeoutMs elapses.
    // Returns true if all holds were released, false on timeout.
    // Called by the init thread after InitAllLoadedPlugins completes.
    bool WaitForAllHolds(DWORD timeoutMs);

    // Close the splash window and clean up the background thread.
    // Blocks briefly until the window thread exits.
    void Close();
}
