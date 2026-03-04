#pragma once

// ---------------------------------------------------------------------------
// Splash Window — client-only startup progress overlay
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
    // Thread-safe — can be called from any thread.
    void SetStatus(const wchar_t* text);

    // Update the progress bar (0.0 – 1.0).
    // Thread-safe — can be called from any thread.
    void SetProgress(float fraction);

    // Close the splash window and clean up the background thread.
    // Blocks briefly until the window thread exits.
    void Close();
}
