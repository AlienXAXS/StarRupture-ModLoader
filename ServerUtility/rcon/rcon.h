#pragma once

// Main entry point for the RCON / Steam Query subsystem.
//
// Call Init()     from OnEngineInit (after Winsock is available).
// Call Shutdown() from OnEngineShutdown (before UObject teardown).
//
// Player state is refreshed every 5 seconds by the background thread using
// SDK::UWorld::GetWorld() -- no world-event callbacks required.
namespace Rcon
{
	// Reads -QueryPort= and -RconPassword=, starts TCP + UDP servers,
	// registers commands, and launches the background player-refresh thread.
	void Init();

	// Stops all servers and the refresh thread; cleans up Winsock.
	void Shutdown();
}
