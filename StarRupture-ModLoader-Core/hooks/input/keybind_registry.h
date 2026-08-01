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

	// --- Callback registration (by name string, plain or combo) ---
	// Accepts "F5" or combo strings like "Ctrl+C", "Shift+F5", "Ctrl+Shift+Delete".
	// Routes to plain or combo storage automatically; callback signature is the same either way.
	void RegisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback);
	void UnregisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback);

	// Called by the config UI when a Keybind config entry changes value.
	// Finds all by-name registrations for pluginName whose stored combo matches
	// oldCombo and re-registers them with newCombo in-place.
	void UpdateKeybindByName(const char* pluginName, const char* oldCombo, const char* newCombo);

	// --- Advanced combo registration (v28) ---
	// Register by enum + modifier mask; callback receives the mods at fire time.
	void RegisterKeybindCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback);
	void UnregisterKeybindCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback);

	// --- Lookup helpers ---

	// Returns the Win32 VK code for a given EModKey, or 0 if unknown.
	int ModKeyToVK(EModKey key);

	// Returns the EModKey for a Win32 VK code, or EModKey::Unknown.
	EModKey VKToModKey(int vk);

	// Returns the UE key name string for a given EModKey, or nullptr if unknown.
	const char* ModKeyToName(EModKey key);

	// Returns the EModKey for a UE key name string (case-insensitive), or EModKey::Unknown.
	EModKey NameToModKey(const char* name);

	// Returns the current modifier bitmask by sampling GetAsyncKeyState.
	EModKeyModifiers SampleCurrentModifiers();

	// Formats a combo string into outBuf (e.g. "Ctrl+F5"). Returns outBuf.
	// mods == EModKeyMod_None produces just the key name.
	const char* FormatComboString(EModKey key, EModKeyModifiers mods, char* outBuf, size_t outLen);

	// --- Blocking ---
	// Mark a canonical combo string (e.g. "Ctrl+C", "F5") as blocking or non-blocking.
	// When blocking is true, the InputKey detour returns false for that combo,
	// preventing UE5 from processing the key. Stored per-combo in a runtime map;
	// persisted to the plugin INI by the config UI.
	void SetComboBlocking(const char* comboStr, bool blocking);

	// Returns true if the given (key, mods) combination is currently set to blocking.
	// Uses FormatComboString to produce the canonical key and looks it up in the map.
	// Returns false (non-blocking) by default when no entry exists.
	bool ShouldBlock(EModKey key, EModKeyModifiers mods);

	// --- Typing exemptions ---
	// By default a keybind does not fire while a game or ImGui text field has
	// focus, so typing "Port" into a box cannot trigger a plugin bound to "P".
	// Escape is always exempt; register additional keys here when a toggle key
	// must be able to dismiss the very window whose text box is focused (the
	// developer console's open key). Add on open, remove on close, so the key
	// still types normally everywhere else.
	void SetTypingExempt(EModKey key, bool exempt);
	bool IsTypingExempt(EModKey key);

	// --- Dispatch ---
	// Fires simple callbacks. These carry no modifiers of their own and so fire
	// whatever is held, but `mods` is still needed: a bind registered for exactly
	// the modifiers currently down shadows them, so Ctrl+F7 does not also fire a
	// plugin that only asked for F7.
	void Dispatch(EModKey key, EModKeyModifiers mods, EModKeyEvent event);
	// Fires named-combo and advanced-combo callbacks for the given key + current mods.
	// Modifier-less named entries follow the same shadowing rule as Dispatch.
	void DispatchCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event);

	// Returns the set of (EModKey, VK) pairs that have at least one registered callback
	// (simple or combo).  Used by the input processor to know which keys to poll.
	std::vector<std::pair<EModKey, int>> GetActiveKeys();

	// --- Diagnostics ---
	// How many callbacks are registered, split by kind, plus how many combos are
	// currently set to swallow their key from the game. For the debug HUD: a
	// keybind count that keeps climbing across plugin reloads is a plugin
	// re-registering without unregistering, and a blocking entry that outlives
	// the plugin that set it is why a key stopped reaching the game.
	void GetRegistrationCounts(int* outSimple, int* outNamedCombos,
	                           int* outAdvancedCombos, int* outBlocking);

	// The combos currently set to blocking, comma-separated, into a caller buffer.
	//
	// The count alone is not actionable: a blocking entry swallows its key before
	// UE ever sees it, so "3 keys blocked" leaves the player guessing which three
	// stopped working. Naming them turns "my character will not walk" into "W is
	// blocked" in one glance, which is the difference between a readout and a
	// diagnosis.
	void GetBlockedCombos(char* out, int outSize);

	// Process a WM_KEY*/WM_MOUSE* window message: translate it to an EModKey,
	// fire all registered keybind callbacks (simple + combo), and return true if
	// the message should be swallowed (blocking mode is set for this combo).
	// Auto-repeat (WM_KEYDOWN with lParam bit 30 set) is silently ignored.
	// Used by the ImGui host DLL WndProc hook as a callback into the main DLL.
	bool ProcessWindowMessage(UINT msg, WPARAM wParam, LPARAM lParam);
}
