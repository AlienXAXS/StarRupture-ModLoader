#include "pch.h"
#include "global_settings.h"

#ifdef MODLOADER_CLIENT_BUILD

#include <cstring>
#include <cstdio>
#include <imgui.h>

// Forward declaration to avoid including imgui_backend.h (which is a higher-level
// header that does not include global_settings.h -- keeping the dependency one-way).
namespace UI::ImGuiBackend { void RequestFontRebuild(); }

namespace UI::GlobalSettings
{
    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    static wchar_t s_iniPath[MAX_PATH] = {};

    // Toggles
    static bool  s_showFPS            = false;
    static bool  s_showWorldName      = false;
    static bool  s_showPlayerPosition = false;
    static bool  s_showDebugValues     = false;
    static float s_fontScale          = 1.0f;
    static char  s_fontFamily[32]     = "Default";

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
        s_showDebugValues    = ReadBool(L"HUD", L"ShowDebugValues",    false);

        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"UI", L"FontScale", L"1.00", buf, 32, s_iniPath);
        float parsed = 1.0f;
        if (swscanf_s(buf, L"%f", &parsed) == 1)
        {
            if (parsed < 0.75f) parsed = 0.75f;
            if (parsed > 5.00f) parsed = 5.00f;
        }
        s_fontScale = parsed;
        // ImGui context is not yet created here; scale is applied in InitD3D12Resources().

        // Font family: if the user has ever picked one in the theme settings
        // it's stored in the INI and always wins (including an explicit
        // "Default"). Only when the key is completely absent do we prefer
        // Arial -- it renders noticeably better than the ImGui built-in --
        // falling back to the built-in if arial.ttf isn't on this system.
        wchar_t familyBuf[32] = {};
        GetPrivateProfileStringW(L"UI", L"FontFamily", L"", familyBuf, 32, s_iniPath);
        if (familyBuf[0] == L'\0')
        {
            const bool hasArial =
                GetFileAttributesW(L"C:\\Windows\\Fonts\\arial.ttf") != INVALID_FILE_ATTRIBUTES;
            wcscpy_s(familyBuf, hasArial ? L"Arial" : L"Default");
        }
        // Convert narrow ASCII key -- all valid keys are plain ASCII
        char narrowBuf[32] = {};
        for (int i = 0; i < 31 && familyBuf[i]; ++i)
            narrowBuf[i] = static_cast<char>(familyBuf[i]);
        strncpy_s(s_fontFamily, narrowBuf, _TRUNCATE);
    }

    void Save(const wchar_t* /*iniPath*/)
    {
        WriteBool(L"HUD", L"ShowFPS",            s_showFPS);
        WriteBool(L"HUD", L"ShowWorldName",      s_showWorldName);
        WriteBool(L"HUD", L"ShowPlayerPosition", s_showPlayerPosition);
        WriteBool(L"HUD", L"ShowDebugValues",    s_showDebugValues);

        wchar_t buf[32] = {};
        swprintf_s(buf, L"%.2f", s_fontScale);
        WritePrivateProfileStringW(L"UI", L"FontScale", buf, s_iniPath);

        wchar_t familyBuf[32] = {};
        for (int i = 0; i < 31 && s_fontFamily[i]; ++i)
            familyBuf[i] = static_cast<wchar_t>(s_fontFamily[i]);
        WritePrivateProfileStringW(L"UI", L"FontFamily", familyBuf, s_iniPath);
    }

    const char* GetFontFamily() { return s_fontFamily; }

    void SetFontFamily(const char* key)
    {
        if (!key) key = "Default";
        strncpy_s(s_fontFamily, key, _TRUNCATE);
        Save(nullptr);
        UI::ImGuiBackend::RequestFontRebuild();
    }

    float GetFontScale() { return s_fontScale; }

    void SetFontScale(float scale)
    {
        if (scale < 0.75f) scale = 0.75f;
        if (scale > 5.00f) scale = 5.00f;
        s_fontScale = scale;
        Save(nullptr);
        ImGui::GetStyle().FontScaleMain = s_fontScale;
    }

    bool GetShowFPS()            { return s_showFPS; }
    bool GetShowWorldName()      { return s_showWorldName; }
    bool GetShowPlayerPosition() { return s_showPlayerPosition; }
    bool GetShowDebugValues()    { return s_showDebugValues; }

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

    void SetShowDebugValues(bool v)
    {
        s_showDebugValues = v;
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
