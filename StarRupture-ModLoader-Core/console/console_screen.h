#pragma once

#include <windows.h>

#include <string>

#include "console/console_commands.h"

// ---------------------------------------------------------------------------
// ConsoleScreen -- owns everything drawn in the -console window
//
// The console has one rule: the bottom row belongs to the input line and
// nothing else is ever written there. Every piece of output erases the prompt,
// prints above it and draws it again, so a half-typed command survives any
// amount of log spam -- including the engine's, which arrives on its own
// threads and knows nothing about us (see engine_output_hook.h for how it is
// intercepted).
//
// That is also why this module owns the console handle rather than the reader
// thread: prompt state and output have to be serialised against each other, and
// they come from different threads.
//
// Colour is applied per line by severity and per word by the highlight table in
// console_screen.cpp -- both plain SetConsoleTextAttribute, no VT sequences, so
// nothing depends on the console's mode surviving whatever the engine does to
// it.
// ---------------------------------------------------------------------------

namespace ConsoleScreen
{
    // Takes the console output handle. Does not own it -- ServerConsole opened
    // it and closes it.
    bool Init(HANDLE out);
    void Shutdown();
    bool IsActive();

    // One line of mod loader output, coloured by kind.
    void Print(ModConsole::LineKind kind, const char* text);

    // Raw text captured from the engine's own console writes. Partial lines are
    // buffered until their newline arrives, so a line split across two writes is
    // still colourised as one.
    void PrintEngineText(const wchar_t* text, size_t len);

    // Bottom line contents. The input editor calls this on every keystroke;
    // caret is a byte offset into input.
    void SetPrompt(const char* prompt, const std::string& input, size_t caret);

    // Re-draw the bottom line, e.g. after a window resize.
    void RedrawPrompt();

    void ClearScreen();

    // Current console width in columns, for the editor's horizontal scrolling.
    int Width();
}
