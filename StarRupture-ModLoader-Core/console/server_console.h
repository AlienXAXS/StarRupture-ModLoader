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

    // Ask the engine to shut down cleanly, by raising a console Ctrl+C event
    // that we deliberately let through to the engine's own handler.
    //
    // UE installs a graceful termination handler on the process console
    // (FWindowsPlatformMisc::SetGracefulTerminationHandler) whose whole job is
    // to flush the log and call RequestEngineExit -- the engine then finishes
    // the current tick, runs its normal shutdown and exits. Ctrl+C is the
    // canonical way to stop a UE dedicated server for exactly this reason, so
    // `stop` raises one rather than inventing a second shutdown path.
    //
    // Returns false when there is no console to raise the event on, which is
    // also the only case where nothing at all happens.
    //
    // If the engine's handler turns out not to be installed, the event falls
    // through to the system default, which terminates the process -- the server
    // still stops, just without the clean shutdown. There is no way to tell the
    // two apart from here, so `stop` reports what it asked for, not what
    // happened.
    //
    // Note the event goes to every process attached to this console. When the
    // server was launched from a batch file, that shell sees the Ctrl+C too
    // (the usual "Terminate batch job?" prompt) -- the same as pressing Ctrl+C
    // in the window by hand.
    bool RequestGracefulProcessExit();
}
