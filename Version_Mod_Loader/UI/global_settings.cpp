#include "pch.h"
#include "global_settings.h"

#ifdef MODLOADER_CLIENT_BUILD

#include <cstring>
#include <cstdio>

namespace UI::GlobalSettings
{
    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    static wchar_t s_iniPath[MAX_PATH] = {};

    // Toggles
    static bool s_showFPS            = false;
    static bool s_showWorldName      = false;
    static bool s_showPlayerPosition = false;

    // Live data -- written on game thread, read on render thread.
    static char   s_worldName[128]      = {};
    static double s_posX                = 0.0;
    static double s_posY                = 0.0;
    static double s_posZ                = 0.0;
    static bool   s_posValid            = false;

    // Update notification -- written by auto-updater thread, read by render thread.
    // A plain bool write/read is atomic on x86-64 so no mutex is needed here.
    static bool   s_updateAvailable     = false;

    // -----------------------------------------------------------------------
    // INI helpers
    // -----------------------------------------------------------------------
    static bool ReadBool(const wchar_t* section, const wchar_t* key, bool defaultVal)
    {
        return GetPrivateProfileIntW(section, key, defaultVal ? 1 : 0, s_iniPath) != 0;
    }

    static void WriteBool(const wchar_t* section, const wchar_t* key, bool v)
    {
        WritePrivateProfileStringW(section, key, v ? L"1" : L"0", s_iniPath);
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------
    void SetIniPath(const wchar_t* iniPath)
    {
        if (iniPath)
            wcsncpy_s(s_iniPath, iniPath, _TRUNCATE);
    }

    const wchar_t* GetIniPath()
    {
        return s_iniPath;
    }

    void Load(const wchar_t* iniPath)
    {
        SetIniPath(iniPath);
        s_showFPS            = ReadBool(L"HUD", L"ShowFPS",            false);
        s_showWorldName      = ReadBool(L"HUD", L"ShowWorldName",      false);
        s_showPlayerPosition = ReadBool(L"HUD", L"ShowPlayerPosition", false);
    }

    void Save(const wchar_t* /*iniPath*/)
    {
        WriteBool(L"HUD", L"ShowFPS",            s_showFPS);
        WriteBool(L"HUD", L"ShowWorldName",      s_showWorldName);
        WriteBool(L"HUD", L"ShowPlayerPosition", s_showPlayerPosition);
    }

    bool GetShowFPS()            { return s_showFPS; }
    bool GetShowWorldName()      { return s_showWorldName; }
    bool GetShowPlayerPosition() { return s_showPlayerPosition; }

    void SetShowFPS(bool v)
    {
        s_showFPS = v;
        Save(nullptr);
    }

    void SetShowWorldName(bool v)
    {
        s_showWorldName = v;
        Save(nullptr);
    }

    void SetShowPlayerPosition(bool v)
    {
        s_showPlayerPosition = v;
        Save(nullptr);
    }

    void SetWorldName(const char* name)
    {
        if (name)
            strncpy_s(s_worldName, name, _TRUNCATE);
        else
            s_worldName[0] = '\0';
    }

    const char* GetWorldName()
    {
        return s_worldName;
    }

    void SetPlayerPosition(double x, double y, double z, bool valid)
    {
        s_posX     = x;
        s_posY     = y;
        s_posZ     = z;
        s_posValid = valid;
    }

    void GetPlayerPosition(double* x, double* y, double* z, bool* valid)
    {
        if (x)     *x     = s_posX;
        if (y)     *y     = s_posY;
        if (z)     *z     = s_posZ;
        if (valid) *valid = s_posValid;
    }

    void SetUpdateAvailable(bool available) { s_updateAvailable = available; }
    bool GetUpdateAvailable()               { return s_updateAvailable; }
}

#endif // MODLOADER_CLIENT_BUILD
