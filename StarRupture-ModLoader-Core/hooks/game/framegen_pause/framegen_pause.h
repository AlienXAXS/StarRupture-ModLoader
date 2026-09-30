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
// state), with r.Streamline.DLSSG.RetainResourcesWhenOff turned on first so
// switching off never frees buffers that frames in flight still read. Both
// go back to what they were when DLSS-G is restored. Nothing changes while a
// world is loading or travelling, or while ticks are long: the wanted state
// is held and applied once a world has begun play and the engine has ticked
// steadily for a second. Call Tick() once per engine tick with the current
// "should pause" state; it only touches the cvars on a change.
// ---------------------------------------------------------------------------

namespace Hooks::FrameGenPause
{
    // wantPaused: true while at least one overlay window is open and the
    // "Pause frame generation while windows are open" setting is on.
    // deltaSeconds: this tick's length, used to hold changes through hitches.
    void Tick(bool wantPaused, float deltaSeconds);

    // Registers the world begin/end and engine shutdown callbacks. Call once.
    void Initialize();
}

#endif // MODLOADER_CLIENT_BUILD
