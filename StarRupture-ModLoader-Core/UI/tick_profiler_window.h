#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// TickProfilerWindow
//
// Displays live per-tick timing data recorded by the EngineTick hook's
// stutter detector (see hooks/game/engine_tick/tick_profiler.h): a rolling
// graph of recent tick times, plus an expandable list of every tick flagged
// as a stutter with a breakdown of where the time went (the engine's own
// Tick() call, GameThreadDispatch::Drain, and each plugin's tick callback).
//
// Opened via a button in the F2 menu's Settings > Diagnostics section.
// Data collection itself is driven by the "Log Tick Stutters" toggle in that
// same section -- this window only visualizes what's already being recorded.
// ---------------------------------------------------------------------------
namespace UI::TickProfilerWindow
{
    void Open();
    void Toggle();
    bool IsOpen();

    // Render this frame. Call inside an active ImGui frame; renders nothing
    // when closed.
    void Render();
}

#endif // MODLOADER_CLIENT_BUILD
