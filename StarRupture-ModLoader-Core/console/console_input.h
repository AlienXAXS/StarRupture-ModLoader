#pragma once

#include <windows.h>

#include <string>

// ---------------------------------------------------------------------------
// ConsoleInput -- raw-mode line editor for the -console window
//
// The console's own line input (ENABLE_LINE_INPUT) echoes at the cursor and
// owns the line until Enter, which makes a prompt that survives concurrent
// output impossible: any write lands in the middle of what is being typed and
// the text is gone. So input is read as key events and the line is drawn by
// ConsoleScreen, which can erase and redraw it around every piece of output.
//
// Re-implementing the editing the console gave us for free is the price. What
// it buys, beyond the sticky prompt: history, word movement, and tab
// completion over the command registry and the loaded plugin names.
//
// Mouse selection is left to the console (QuickEdit stays on) so log text can
// still be selected and copied.
// ---------------------------------------------------------------------------

namespace ConsoleInput
{
    // Puts the console into raw input mode. Returns false if the mode could not
    // be set, in which case the caller should not use ReadLine.
    bool Begin(HANDLE in);

    // Restores the previous console input mode.
    void End();

    enum class Result
    {
        Line,        // outLine holds a submitted command
        Interrupted, // the console went away; stop reading
    };

    // Blocks until a line is submitted. Draws the prompt as it goes.
    Result ReadLine(std::string& outLine);
}
