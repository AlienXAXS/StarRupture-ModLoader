#pragma once

namespace Hooks::FakePlayer
{
	// Install the hook to spawn a fake player
	bool Install();
	
	// Remove the hook
	void Remove();
	
	// Get call count
	long GetCallCount();
	
	// Check if fake player is active
	bool IsPlayerActive();
	
	// Manually spawn/despawn the fake player
	void SpawnFakePlayer();
	void DespawnFakePlayer();

	// Get the fake player's controller pointer (nullptr if not active)
	void* GetFakeController();

	// Enable/disable debug visible mode (must be set before spawning)
	void SetDebugVisibleMode(bool enabled);

	// Map traversal — teleports fake player through a grid of waypoints
	void StartMapTraversal();
	void StopMapTraversal();
	bool IsTraversing();

	// Called each engine tick to advance traversal (teleport to next waypoint)
	void TickTraversal();
}
