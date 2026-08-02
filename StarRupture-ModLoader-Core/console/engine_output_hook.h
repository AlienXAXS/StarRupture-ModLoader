#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// EngineOutputHook -- routes the engine's console writes through ConsoleScreen
//
// With -log the engine owns a console output device of its own
// (FWindowsConsoleOutputDevice) that calls WriteConsoleW directly, on whatever
// thread happened to log. It knows nothing about our prompt, so every engine
// log line would scribble over the half-typed command on the bottom row.
//
// Detouring WriteConsoleW puts all console output through one place: the
// prompt is erased and redrawn around each line, and the line can be coloured
// on the way past. It is installed only while the -console window is up, and
// removed on shutdown.
//
// Anything drawing the console must use RawWriteW rather than calling
// WriteConsoleW, or it re-enters the detour it just came from.
// ---------------------------------------------------------------------------

namespace EngineOutputHook
{
    // Detour WriteConsoleW. Safe to call more than once.
    bool Install();

    void Remove();

    bool IsInstalled();

    // Write to the console bypassing the detour -- the original function when
    // hooked, WriteConsoleW itself when not.
    BOOL RawWriteW(HANDLE out, const wchar_t* text, DWORD length, DWORD* written);
}
