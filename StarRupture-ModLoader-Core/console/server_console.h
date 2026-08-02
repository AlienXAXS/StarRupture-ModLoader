#pragma once

// ---------------------------------------------------------------------------
// ServerConsole -- a Win32 console window driving the ModConsole registry
//
// Dedicated server builds have no ImGui overlay, so there was no way to reload
// a plugin without restarting the whole game. Launch with -console and this
// puts up a console window with a prompt wired to the same command registry the
// client's ImGui console uses (console/console_commands.h), so `plugins`,
// `reload <name>` and friends work identically on both.
//
// Opt-in, and available on every build target: the client already has its
// in-game console, but a real console window is also the only thing that talks
// while the game is a black screen.
//
// Commands that touch plugins are posted to the game thread by ModConsole, so
// the reader thread here never runs PluginInit itself. It waits for completion
// before printing the next prompt, which keeps output and prompt from
// interleaving.
//
// Note the console shares the process's single console: when the game was
// started with -log the engine already owns one, and we attach to that rather
// than failing. Engine log output and our prompt then share the window.
// ---------------------------------------------------------------------------

namespace ServerConsole
{
    // True when -console (or -mlconsole) is on the command line.
    bool IsRequested();

    // Create the console window and start the reader thread. No-op when the
    // console was not requested or is already running. Safe to call once the
    // logger and plugin manager exist.
    void Start();

    // Close the console window and stop accepting commands. The reader thread
    // is not joined -- it is parked in ReadConsole and cannot be interrupted;
    // detaching the console makes its next read fail and the thread exits.
    void Shutdown();

    // True while the console window is up.
    bool IsRunning();
}
