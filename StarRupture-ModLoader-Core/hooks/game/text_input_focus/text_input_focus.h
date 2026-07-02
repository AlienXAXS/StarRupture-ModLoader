#pragma once

// ---------------------------------------------------------------------------
// Game text-input focus detection (client only)
//
// Answers "does any UMG editable-text widget currently have keyboard focus?"
// so keybind dispatch can be suppressed while the player is typing into a
// game text field (chat box, save name, etc.).  Without this, a plugin bound
// to a letter key (e.g. "P") would fire while the player types that letter
// into a game text box.
//
// Detection walks GObjects comparing UClass pointers against the four stock
// UMG editable-text classes (UEditableText, UEditableTextBox,
// UMultiLineEditableText, UMultiLineEditableTextBox) and asks each live
// instance for keyboard focus via the reflected UWidget::HasKeyboardFocus.
// The current SDK dump has no game-specific subclasses of any of these; if
// the game ever adds one it must be added to the class list in the .cpp.
//
// Cost: one pointer-compare pass over GObjects plus a ProcessEvent call per
// live editable widget (typically 0-3).  Callers keep this off the hot path
// by only querying for keys that actually have a registered keybind.
// ---------------------------------------------------------------------------

namespace Hooks::TextInputFocus
{
	// Read [Input] SuppressKeybindsWhileTyping from modloader.ini (default 1,
	// written back if absent so users can discover the toggle).  Call once at
	// startup before keybind dispatch begins.
	void LoadConfig(const wchar_t* iniPath);

	// True when suppression is enabled, the engine is initialized, and a live
	// UMG editable-text widget currently has keyboard focus.  Returns false
	// before engine init.  Must be called on the game thread (calls
	// ProcessEvent on game objects).
	bool IsGameTextInputFocused();
}
