#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <string>

// ---------------------------------------------------------------------------
// Console Command execution
//
// Runs a console command through APlayerController::ConsoleCommand and returns
// whatever the engine wrote back, so the ModLoader's own console window can
// display output inline.
//
// Why we call this rather than enabling the game's built-in console: on this
// Shipping build the UConsole class is fully compiled in, but every piece of
// engine glue that drives it is not. UGameViewportClient::InputKey has no
// ViewportConsole dispatch, UConsole::PostRender_Console has zero callers
// anywhere in the binary, and UInputSettings::ConsoleKeys is only ever touched
// by UInputSettings itself. So constructing a UConsole and assigning it to
// GameViewport->ViewportConsole (what UE4SS's ConsoleEnablerMod does) produces
// an object nothing ever calls or draws. Command *execution* is untouched
// though, which is all we actually need.
//
// APlayerController::ConsoleCommand returns an FString by value, so the MSVC
// x64 ABI puts a hidden return pointer in RDX:
//   FString* __fastcall ConsoleCommand(APlayerController* this,
//                                      FString* outResult,
//                                      const FString* Command,
//                                      bool bWriteToLog)
// The returned FString's buffer is engine-allocated and is freed through
// EngineAllocator once copied out.
// ---------------------------------------------------------------------------

namespace Hooks::ConsoleCommand
{
    // Resolve APlayerController::ConsoleCommand by AOB. Safe to call more than
    // once; only scans on the first call.
    bool Resolve();

    // True once Resolve() has succeeded.
    bool IsAvailable();

    // Execute a command on the local player controller (index 0).
    // Any output the engine returned is written to outResult (may be empty --
    // plenty of commands succeed silently).
    // Returns false if the function is unresolved, there is no world/player
    // controller yet, or the call faulted.
    bool Execute(const wchar_t* command, std::wstring& outResult);
}

#endif // MODLOADER_CLIENT_BUILD
