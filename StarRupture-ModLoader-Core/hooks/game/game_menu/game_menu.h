#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Main menu / pause menu entry injection
//
// Adds rows to the game's own menus -- the loader's "MOD LOADER" row sits
// between OPTIONS and CREDITS on the main menu -- so the overlay is reachable
// without knowing that F2 exists.  What appears is decided by
// game_menu_registry.h; this file is the engine side of it.
//
// How the menus are built
// -----------------------
// UCrUW_MainMenuWidget and UCrUW_PauseMenu are the same shape, and both are
// entirely data-driven:
//
//   ButtonsBox->ClearChildren()
//   for (It : LeftButtons) {
//       btn = CreateWidgetInstance(GetOwningPlayer(), LeftButtonClass, NAME_None)
//       btn->SetIndex(Index)
//       if (ButtonsTexts.Find(It)) btn->SetButtonText(*found)
//       ButtonsIndexes.Emplace(Index, It)
//       btn->AddOnTabClicked(&ThisClass::ButtonClicked)
//       ButtonsBox->AddChild(btn); ++Index
//   }
//
// So CONTINUE is ContinueButton, QUIT is ExitButton, and everything between
// them is LeftButtons in order -- and those are the only children of
// ButtonsBox.  Splicing a value into LeftButtons before that loop runs makes
// the engine build the button, size it, sound it, wire the click and shift
// every index after it, with no widget construction on our side at all.
//
// Where the two menus differ is the numbering, and only the numbering:
//
//   MainMenuWidget  ContinueButton->SetIndex(0) and Index = 1 BEFORE the loop,
//                   ExitButton->SetIndex(Num + 1) after it.
//   PauseMenu       Index = 0 before the loop; ContinueButton, ExitButton and
//                   ExitToMainMenuButton take Num, Num + 1 and Num + 2 AFTER
//                   it.
//
// so a row's index is its LeftButtons position plus a per-menu base.  Reading
// that back with the wrong base is a silent failure, not a crash -- see
// kMainIndexBase / kPauseIndexBase in game_menu.cpp.
//
// The three things that make this safe
// ------------------------------------
//  1. The spliced value is a sentinel ECrMenuType in 0xC0..0xFF.  ECrMenuType
//     is a uint8 whose real values stop at 19, so the range is one the game
//     cannot produce.  The ButtonsTexts lookup therefore misses, which sets no
//     text rather than crashing -- we set the label ourselves afterwards.
//
//  2. ButtonClicked is detoured and swallows the click for a sentinel row,
//     returning without calling the original.  The original would look the
//     index up in ButtonsIndexes and broadcast OnMenuActionTriggeredDelegate,
//     and UCrUW_PauseMenuMainScreen::OnActionTriggered's switch would land in
//     its default case with a value it has never seen.  It must never get
//     there, so the sentinel stops here.
//
//  3. Which entry was clicked is read back out of the widget's own
//     LeftButtons array (index N maps to LeftButtons[N - base]), not from a
//     table of remembered UTabButton pointers.  The widget is live -- it is
//     the `this` of the call -- whereas a remembered button pointer outlives
//     the menu it belonged to and a recycled UObject address at the same
//     location would look like a match.
//
// Both hooks are optional.  A pattern miss disables menu injection and logs
// it; it does not fail preflight, because a button that opens a window the
// user can already open with a key is not worth refusing to boot over.
// ---------------------------------------------------------------------------

namespace SDK { class APlayerController; }

namespace Hooks::GameMenu
{
	// Resolve and install both menus' hooks.  Must be called after engine init
	// -- ButtonClicked is located through its UFunction, which needs GObjects.
	// Returns true when at least one menu was hooked.
	bool Install();

	// Remove whatever was installed.
	void Remove();

	// True when at least one menu is hooked and entries will appear.
	bool IsInstalled();

	// Samples whether the pause menu (the Escape screen) is the active
	// screen.  The pause hook remembers the widget it constructed; this
	// checks GObjects still holds it and reads its CommonUI activation flag.
	// Game thread only -- call it once per tick.
	void Tick();

	// What the last Tick saw.  Safe from any thread.  False when the pause
	// hook is not installed.
	bool IsPauseMenuOpen();

	// Whether the local player controller has the mouse cursor up (any game
	// screen that shows one: the pause menu, machine menus, the inventory).
	// Set once per tick on the game thread by the caller that already looks up
	// the controller; false when there is no world or controller.  Safe from
	// any thread.
	void SetGameCursorShown(bool shown);
	bool IsGameCursorShown();

	// The menu-layer bits of the plugins' UI state (PluginUIState: ANY_MENU,
	// and MAP, INVENTORY, BUILD_MENU or STATION_MENU when the screen is one of
	// those) for this controller's local player, read from the game's CommonUI
	// menu layer. 0 when nothing is on it; a screen that is closing still
	// counts, until its close transition has ended. Game thread only.
	uint64_t ReadMenuLayer(SDK::APlayerController* pc);

	// Whether the cursor is over a visible, hit-testable widget of that same
	// menu layer (UWidget::IsHovered on each screen on it). True when the layer
	// is empty or not found, since the loader then cannot tell where the menu's
	// widgets are. Game thread only; no pointer outlives the call.
	bool ReadMenuHover(SDK::APlayerController* pc);

	// The last published answer of ReadMenuHover, for the window thread that
	// decides which widgets take the mouse. Set once per game tick, so it can
	// be a frame old. Safe from any thread.
	void SetMenuWidgetHovered(bool hovered);
	bool IsMenuWidgetHovered();
}

#endif // MODLOADER_CLIENT_BUILD
