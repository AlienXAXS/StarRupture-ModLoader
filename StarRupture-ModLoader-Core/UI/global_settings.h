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

    // Retrieve the stored ini path (e.g. for persisting other settings).
    const wchar_t* GetIniPath();

    // -----------------------------------------------------------------------
    // Settings toggles (read from render thread, written from UI thread)
    // -----------------------------------------------------------------------
    bool GetShowFPS();
    void SetShowFPS(bool v);

    bool GetShowWorldName();
    void SetShowWorldName(bool v);

    bool GetShowPlayerPosition();
    void SetShowPlayerPosition(bool v);

    // Adds a block of modloader-internal state to the HUD info box: outstanding
    // input tokens and who holds them, what the input arbitration currently
    // resolves to, and what ImGui thinks it owns this frame.
    //
    // Separate from the other HUD toggles because it is for diagnosing the
    // modloader rather than for looking at the game -- specifically the class of
    // bug where a plugin holds input open and the player has no way to see it.
    bool GetShowDebugValues();
    void SetShowDebugValues(bool v);

    float GetFontScale();
    void  SetFontScale(float scale);

    // Font family key stored in modloader.ini [UI] FontFamily.
    // Returns one of the iniKey strings from the curated font list
    // (e.g. "Default", "Arial", "YaHei", "Meiryo", "Malgun", "ArialCJK").
    const char* GetFontFamily();
    void        SetFontFamily(const char* key);

    // -----------------------------------------------------------------------
    // Live data (written from game thread, read from render thread)
    // -----------------------------------------------------------------------
    void        SetWorldName(const char* name);
    const char* GetWorldName();

    // x/y/z in Unreal units (cm).  valid=false when no pawn is available.
    void SetPlayerPosition(double x, double y, double z, bool valid);
    void GetPlayerPosition(double* x, double* y, double* z, bool* valid);

    // -----------------------------------------------------------------------
    // Update notification (written by auto-updater thread, read by render thread)
    // -----------------------------------------------------------------------

    // Set to true when the auto-updater detects the local version is behind
    // the remote manifest.  Displayed as a red warning in the overlay.
    void SetUpdateAvailable(bool available);
    bool GetUpdateAvailable();
}

#endif // MODLOADER_CLIENT_BUILD
