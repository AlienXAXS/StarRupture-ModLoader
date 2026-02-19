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

	// Enable/disable debug visible mode (must be set before spawning)
	void SetDebugVisibleMode(bool enabled);
}
