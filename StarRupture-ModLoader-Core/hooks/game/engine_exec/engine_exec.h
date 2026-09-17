#pragma once

#include <string>

// ---------------------------------------------------------------------------
// Engine console command execution without a player controller
//
// The client's developer console runs commands through
// APlayerController::ConsoleCommand (hooks/game/console_command), which is
// the route the engine itself uses for typed input -- but it needs a local
// PlayerController, and a dedicated server never has one. That is why the
// -console window used to answer "Unknown command" to anything that was not
// a mod loader command: there was nothing to forward it to.
//
// This calls UGameEngine::Exec(UWorld*, const TCHAR*, FOutputDevice&) on
// GEngine instead. It is the function the player-controller route ends up in
// anyway (UPlayer::Exec -> ULocalPlayer::Exec -> GEngine->Exec), and its
// StaticExec hands the line to IConsoleManager::ProcessUserConsoleInput first,
// so console variables and FAutoConsoleCommand-registered commands (the
// CrRepGraph.*, Net.*, stat, etc. families) all work with no player involved.
// What it skips is the player-level layer (APlayerController::ProcessConsoleExec,
// cheat manager), which is why the client console keeps its own route and
// only the -console window uses this one.
//
// Built on every target -- the pattern is unique in both binaries -- so the
// -console window behaves the same whichever executable it is attached to.
//
// Output is captured through our own FOutputDevice. Anything the command
// writes to its FOutputDevice& argument comes back in outResult; anything it
// writes to GLog directly (UE_LOG inside the command) goes to the engine log
// as usual, and reaches the -console window only when the engine's own
// console output device is attached (-log).
//
// Must be called on the game thread: UEngine::Exec walks engine state.
// ---------------------------------------------------------------------------

namespace Hooks::EngineExec
{
    enum class Result
    {
        Unavailable,   // pattern unresolved, no GEngine, or the call faulted
        Unhandled,     // engine ran it and nothing claimed the command
        Handled        // a cvar, console command or exec handler took it
    };

    // Resolve UGameEngine::Exec by AOB. Safe to call more than once; only
    // scans on the first call.
    bool Resolve();

    // True once Resolve() has succeeded.
    bool IsAvailable();

    // Execute a command on GEngine against the current world. Output the
    // command wrote to its FOutputDevice is returned in outResult (may be
    // empty -- many commands succeed silently or log through GLog instead).
    Result Execute(const wchar_t* command, std::wstring& outResult);
}
