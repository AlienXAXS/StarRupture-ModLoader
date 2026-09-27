#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// FrameGenPause
//
// DLSS Frame Generation's interpolated frames never pass through the loader's
// Present hook (Streamline composites and presents them itself), so every
// other displayed frame has no ImGui draw on it -- read by the user as
// flicker whenever an overlay window is open. There is no supported way to
// draw into a generated frame; the fix is to turn frame generation off for
// as long as an overlay window is open, and put back whatever it was set to
// once every window is closed.
//
// Toggles the r.Streamline.DLSSG.Enable console variable through
// Hooks::EngineExec::Execute (game thread only -- UEngine::Exec walks engine
// state). Call Tick() once per engine tick with the current "should pause"
// state; it only touches the cvar on a change, never every frame.
// ---------------------------------------------------------------------------

namespace Hooks::FrameGenPause
{
    // wantPaused: true while at least one overlay window is open and the
    // "Pause frame generation while windows are open" setting is on.
    // No-op when wantPaused matches the state from the last call.
    void Tick(bool wantPaused);
}

#endif // MODLOADER_CLIENT_BUILD
