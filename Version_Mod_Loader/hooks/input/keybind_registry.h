#pragma once

#include "../../plugins/plugin_interface.h"
#include <vector>
#include <utility>

// ---------------------------------------------------------------------------
// Keybind Registry (v15, client builds only)
//
// Internal registry used by the input processor and hooks_interface.
// Stores plugin-registered callbacks keyed by (EModKey, EModKeyEvent).
// Provides bidirectional lookups between EModKey, VK code, and UE key name.
// ---------------------------------------------------------------------------

namespace Hooks::Input
{
	// Initialize internal tables (no-op after first call).
	void Initialize();

	// Shutdown: clear all registered callbacks.
	void Shutdown();

	// --- Callback registration (by enum) ---
	void RegisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);
	void UnregisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);

	// --- Callback registration (by UE key name string) ---
	void RegisterKeybindByName(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback);
	void UnregisterKeybindByName(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback);

	// --- Lookup helpers ---

	// Returns the Win32 VK code for a given EModKey, or 0 if unknown.
	int ModKeyToVK(EModKey key);

	// Returns the EModKey for a Win32 VK code, or EModKey::Unknown.
	EModKey VKToModKey(int vk);

	// Returns the UE key name string for a given EModKey, or nullptr if unknown.
	const char* ModKeyToName(EModKey key);

	// Returns the EModKey for a UE key name string (case-insensitive), or EModKey::Unknown.
	EModKey NameToModKey(const char* name);

	// --- Dispatch ---
	// Called by the input processor each frame to fire registered callbacks.
	void Dispatch(EModKey key, EModKeyEvent event);

	// Returns the set of (EModKey, VK) pairs that have at least one registered callback.
	// Used by the input processor to know which keys to poll.
	std::vector<std::pair<EModKey, int>> GetActiveKeys();
}
