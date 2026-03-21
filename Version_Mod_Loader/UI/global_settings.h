#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// GlobalSettings
//
// User-configurable HUD display toggles persisted in modloader.ini [HUD].
// Settings are loaded once at startup and saved immediately on change.
//
// The world-name and player-position values are written from the game thread
// (EngineTick callback) and read from the render thread (ImGui Present hook).
// They are plain scalars written/read without locking -- acceptable for a
// debug HUD where a torn or one-frame-stale value is harmless.
// ---------------------------------------------------------------------------

namespace UI::GlobalSettings
{
    // Load all [HUD] settings from modloader.ini.  Call once at startup.
    void Load(const wchar_t* iniPath);

    // Save all [HUD] settings to modloader.ini.  Called automatically by Set*.
    void Save(const wchar_t* iniPath);

    // Store the ini path so Save() can be called without a parameter later.
    void SetIniPath(const wchar_t* iniPath);

    // -----------------------------------------------------------------------
    // Settings toggles (read from render thread, written from UI thread)
    // -----------------------------------------------------------------------
    bool GetShowFPS();
    void SetShowFPS(bool v);

    bool GetShowWorldName();
    void SetShowWorldName(bool v);

    bool GetShowPlayerPosition();
    void SetShowPlayerPosition(bool v);

    // -----------------------------------------------------------------------
    // Live data (written from game thread, read from render thread)
    // -----------------------------------------------------------------------
    void        SetWorldName(const char* name);
    const char* GetWorldName();

    // x/y/z in Unreal units (cm).  valid=false when no pawn is available.
    void SetPlayerPosition(double x, double y, double z, bool valid);
    void GetPlayerPosition(double* x, double* y, double* z, bool* valid);
}

#endif // MODLOADER_CLIENT_BUILD
