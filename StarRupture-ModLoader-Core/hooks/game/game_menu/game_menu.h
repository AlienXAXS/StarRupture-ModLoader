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
}

#endif // MODLOADER_CLIENT_BUILD
