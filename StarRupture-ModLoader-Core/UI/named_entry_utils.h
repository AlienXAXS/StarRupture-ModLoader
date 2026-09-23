#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// UI::NamedEntryUtils
//
// Small, shared rule for anything the user names and saves to disk in this
// UI: which characters are safe in a name, and how to turn "<base>" into a
// name nothing already has. Pulled out of theme.cpp's Save-As flow (its only
// caller today) so a second in-UI caller wouldn't hand-roll the same two
// rules a plugin's own preset store already has (BetterDrone/BetterCheats'
// preset_store.h/.cpp, not part of this repo).
// ---------------------------------------------------------------------------
namespace UI::NamedEntryUtils
{
    // Rejects an empty name, "." or ".." (filesystem-special), or one
    // containing any of \ / : * ? " < > | (illegal in a Windows filename --
    // callers that store one entry as one file need this; callers that
    // store entries as INI sections under one file don't strictly need to,
    // but the same restriction keeps a name portable between both shapes).
    bool IsValidEntryName(const char* name);

    // Fills out with baseName, or baseName + " 2", " 3", ... -- the first
    // spelling that doesn't case-insensitively match any of
    // existing[0..existingCount). Callers build baseName themselves (e.g.
    // "<current> Custom", or plain "Custom" when nothing is currently
    // selected); this only handles making it unique. Never fails; truncates
    // to outSize if baseName is already long.
    void SuggestUniqueName(const char* baseName, const char* const* existing,
                            int existingCount, char* out, int outSize);
}

#endif // MODLOADER_CLIENT_BUILD
