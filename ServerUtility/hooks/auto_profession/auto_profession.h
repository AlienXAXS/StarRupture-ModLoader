#pragma once

// ---------------------------------------------------------------------------
// Auto-profession patch for GetProfessionForNewPlayer.
//
// When the MaxPlayers patch is active (>4 players), the game's profession
// selection UI can break for extra players. This patches
// ACrGameModeBase::GetProfessionForNewPlayer to always return 1 (Soldier),
// forcing the server to assign a profession immediately so every player
// gets a pawn without relying on the UI.
//
// The patch is a simple prologue overwrite:
//   mov eax, 1   ; B8 01 00 00 00
//   ret   ; C3
// ---------------------------------------------------------------------------

namespace AutoProfessionHook
{
    // Scan for GetProfessionForNewPlayer and patch its prologue.
    // Call once after the scanner is available and MaxPlayers is enabled.
    void Install();

    // Restore the original prologue bytes on shutdown.
    void Remove();
}
