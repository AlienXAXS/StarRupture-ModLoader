#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// Forward declarations for SDK types (avoid including SDK headers in mod loader)
namespace SDK
{
	class UWorld;
}

// ---------------------------------------------------------------------------
// UWorld::EndPlay Hook
// ---------------------------------------------------------------------------

namespace Hooks::WorldEndPlay
{
	// Callback signature for before/after world end play notifications
	using PluginWorldEndPlayCallback = void(*)(SDK::UWorld* world, const char* worldName);

	// Install the hook
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Get call count
	long GetCallCount();

	// Register a callback fired BEFORE the original UWorld::EndPlay runs.
	// Lazily installs the hook on first registration.
	void RegisterBeforeCallback(PluginWorldEndPlayCallback callback);

	// Unregister a before callback
	void UnregisterBeforeCallback(PluginWorldEndPlayCallback callback);

	// Register a callback fired AFTER the original UWorld::EndPlay runs.
	// Lazily installs the hook on first registration.
	void RegisterAfterCallback(PluginWorldEndPlayCallback callback);

	// Unregister an after callback
	void UnregisterAfterCallback(PluginWorldEndPlayCallback callback);
}
