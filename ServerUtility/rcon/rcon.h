#pragma once

// Forward-declare UWorld so callers don't need the full SDK header
namespace SDK { class UWorld; }

// Main entry point for the RCON / Steam Query subsystem.
//
// Call Init()     from OnEngineInit (after Winsock is available).
// Call Shutdown() from OnEngineShutdown (before UObject teardown).
//
// The world/player callbacks must be wired to the mod-loader hooks:
//   RegisterAnyWorldBeginPlayCallback   -> Rcon::OnAnyWorldBeginPlay
//   RegisterExperienceLoadCompleteCallback -> Rcon::OnExperienceLoadComplete
namespace Rcon
{
    // Reads -QueryPort= and -RconPassword=, starts TCP + UDP servers,
    // registers commands, and launches the background player-refresh thread.
    void Init();

    // Stops all servers and the refresh thread; cleans up Winsock.
    void Shutdown();

    // Game-thread callbacks – safe to call from the registered hook callbacks.
    void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName);
    void OnExperienceLoadComplete();
}
