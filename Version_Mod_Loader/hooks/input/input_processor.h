#pragma once

// ---------------------------------------------------------------------------
// Input Processor (v15, client builds only)
//
// Registers an engine-tick callback that polls Win32 GetAsyncKeyState for
// every VK code that has a registered keybind.  On state transitions it
// calls Hooks::Input::Dispatch so plugin callbacks are fired on the game
// thread, once per press/release event.
// ---------------------------------------------------------------------------

namespace Hooks::Input
{
    // Install: registers the per-frame polling callback into the engine tick.
    void InstallInputProcessor();

    // Remove: unregisters the polling callback.
    void RemoveInputProcessor();
}
