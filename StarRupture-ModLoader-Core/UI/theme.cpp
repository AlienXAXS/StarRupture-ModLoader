#include "pch.h"
#include "theme.h"

#ifdef MODLOADER_CLIENT_BUILD

#include <cstring>
#include <cstdio>
#include <cfloat>
#include <string>

#include "core/startup_utils.h"   // GetModLoaderDir() -- ModLoader\Themes\ lives under it
#include "global_settings.h"      // GetTheme()/SetTheme() -- [UI] Theme= persistence

// FindGlyph(), ImFont::FontSize, and ImTextCharFromUtf8() used by
// IconTabBar() for glyph-bbox centering are internal-only APIs -- not
// exported via the public imgui.h. Safe to include here even across the
// StarRupture-ImGui.dll boundary: these are header-defined inline accessors
// over a struct layout shared by both binaries (same imgui version/headers),
// not symbols that need DLL export.
#include <imgui_internal.h>

namespace UI::Theme
{
    // -----------------------------------------------------------------------
    // Accent ramp -- single source of truth.  Matches the cyan/teal used
    // throughout StarRupture's own HUD (shield bar, toggle indicators,
    // panel corner brackets) rather than a generic red/orange cheat-menu look.
    // -----------------------------------------------------------------------
    // Not const -- the Theme tab edits these directly via AccentBasePtr() etc.
    // so changes apply immediately with no extra refresh step.
    static const ImVec4 kDefaultAccent       (0.25f, 0.85f, 0.78f, 1.00f);
    static const ImVec4 kDefaultAccentHover  (0.40f, 0.92f, 0.86f, 1.00f);
    static const ImVec4 kDefaultAccentActive (0.18f, 0.70f, 0.64f, 1.00f);

    static ImVec4 kAccent       = kDefaultAccent;
    static ImVec4 kAccentHover  = kDefaultAccentHover;
    static ImVec4 kAccentActive = kDefaultAccentActive;

    ImU32 AccentColor()      { return ImGui::ColorConvertFloat4ToU32(kAccent); }
    ImU32 AccentColorHover() { return ImGui::ColorConvertFloat4ToU32(kAccentHover); }
    ImVec4 AccentColorVec4(float alpha) { return ImVec4(kAccent.x, kAccent.y, kAccent.z, alpha); }

    ImVec4* AccentBasePtr()   { return &kAccent; }
    ImVec4* AccentHoverPtr()  { return &kAccentHover; }
    ImVec4* AccentActivePtr() { return &kAccentActive; }

    // -----------------------------------------------------------------------
    // Highlight ramp + panel border -- separate from the accent triplet
    // above. The accent alone used to drive both "this is a value/control"
    // (toggle-on, sliders, checkmarks) and "this is selected/hovered"
    // (the active sidebar tab, panel borders) at once, which meant a theme
    // could not give those two different meanings different colors -- the
    // game itself does (orange for values/dividers, cyan for hover/
    // selected, white for panel borders; see the in-game pause menu).
    // Default at the same value as the matching accent entry, so this
    // built-in theme's look is unchanged by these existing at all.
    // -----------------------------------------------------------------------
    static const ImVec4 kDefaultHighlight       = kDefaultAccent;
    static const ImVec4 kDefaultHighlightHover  = kDefaultAccentHover;
    static const ImVec4 kDefaultHighlightActive = kDefaultAccentActive;
    static const ImVec4 kDefaultPanelBorder     = kDefaultAccent;

    static ImVec4 kHighlight       = kDefaultHighlight;
    static ImVec4 kHighlightHover  = kDefaultHighlightHover;
    static ImVec4 kHighlightActive = kDefaultHighlightActive;
    static ImVec4 kPanelBorder     = kDefaultPanelBorder;

    ImU32 HighlightColor()      { return ImGui::ColorConvertFloat4ToU32(kHighlight); }
    ImU32 HighlightColorHover() { return ImGui::ColorConvertFloat4ToU32(kHighlightHover); }
    ImVec4 HighlightColorVec4(float alpha) { return ImVec4(kHighlight.x, kHighlight.y, kHighlight.z, alpha); }

    ImVec4* HighlightBasePtr()   { return &kHighlight; }
    ImVec4* HighlightHoverPtr()  { return &kHighlightHover; }
    ImVec4* HighlightActivePtr() { return &kHighlightActive; }

    ImU32   PanelBorderColor() { return ImGui::ColorConvertFloat4ToU32(kPanelBorder); }
    ImVec4* PanelBorderPtr()  { return &kPanelBorder; }

    static void FormatColor(wchar_t* buf, size_t sz, const ImVec4& c)
    {
        swprintf_s(buf, sz, L"%.6f,%.6f,%.6f,%.6f", c.x, c.y, c.z, c.w);
    }

    static bool ParseColor(const wchar_t* s, ImVec4& out)
    {
        float r, g, b, a;
        if (swscanf_s(s, L"%f,%f,%f,%f", &r, &g, &b, &a) != 4) return false;
        out = ImVec4(r, g, b, a);
        return true;
    }

    // "%S" in a wide swprintf treats its argument as a narrow string and
    // widens it -- every ImGuiCol name, accent key and theme name here is
    // plain ASCII, so a byte-for-byte widen is exact. Centralizes the one
    // buffer/truncation point that four separate call sites used to each
    // declare for themselves.
    static std::wstring ToWide(const char* s)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%S", s);
        return buf;
    }

    // Every named color that lives outside ImGuiStyle.Colors[] -- the accent
    // and highlight triplets plus the panel border -- keyed the same way a
    // [ThemeColors] file keys them. One table drives ApplyColorByName,
    // SaveColors and LoadColors instead of three separate hardcoded lists.
    struct SpecialColorEntry { const char* name; ImVec4* value; };
    static const SpecialColorEntry kSpecialColors[] =
    {
        {"AccentBase",      &kAccent},
        {"AccentHover",     &kAccentHover},
        {"AccentActive",    &kAccentActive},
        {"Highlight",       &kHighlight},
        {"HighlightHover",  &kHighlightHover},
        {"HighlightActive", &kHighlightActive},
        {"PanelBorder",     &kPanelBorder},
    };

    // Applies one named color to the live style -- one of the special colors
    // above, or whichever ImGuiCol GetStyleColorName(i) matches `name`.
    // Shared by LoadColors (file-based) and the compiled-in Star Rupture
    // table below, so a theme file and a built-in theme go through identical
    // logic. Unknown names are silently ignored, same as LoadColors always
    // did for a key an older/newer build doesn't recognise.
    static void ApplyColorByName(const char* name, const ImVec4& c)
    {
        for (const auto& e : kSpecialColors)
            if (strcmp(e.name, name) == 0) { *e.value = c; return; }

        ImGuiStyle& style = ImGui::GetStyle();
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const char* colName = ImGui::GetStyleColorName(i);
            if (colName && strcmp(colName, name) == 0) { style.Colors[i] = c; return; }
        }
    }

    void SaveColors(const wchar_t* iniPath)
    {
        if (!iniPath || !iniPath[0]) return;

        ImGuiStyle& style = ImGui::GetStyle();
        wchar_t valBuf[64];
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const char* name = ImGui::GetStyleColorName(i);
            if (!name) continue;
            FormatColor(valBuf, ARRAYSIZE(valBuf), style.Colors[i]);
            WritePrivateProfileStringW(L"ThemeColors", ToWide(name).c_str(), valBuf, iniPath);
        }

        for (const auto& e : kSpecialColors)
        {
            FormatColor(valBuf, ARRAYSIZE(valBuf), *e.value);
            WritePrivateProfileStringW(L"ThemeColors", ToWide(e.name).c_str(), valBuf, iniPath);
        }
    }

    void LoadColors(const wchar_t* iniPath)
    {
        if (!iniPath || !iniPath[0]) return;

        wchar_t valBuf[64];
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const char* name = ImGui::GetStyleColorName(i);
            if (!name) continue;
            GetPrivateProfileStringW(L"ThemeColors", ToWide(name).c_str(), L"", valBuf, ARRAYSIZE(valBuf), iniPath);
            if (!valBuf[0]) continue;
            ImVec4 c;
            if (ParseColor(valBuf, c)) ApplyColorByName(name, c);
        }

        for (const auto& e : kSpecialColors)
        {
            GetPrivateProfileStringW(L"ThemeColors", ToWide(e.name).c_str(), L"", valBuf, ARRAYSIZE(valBuf), iniPath);
            ImVec4 c;
            if (valBuf[0] && ParseColor(valBuf, c)) ApplyColorByName(e.name, c);
        }
    }

    void ResetColors()
    {
        kAccent       = kDefaultAccent;
        kAccentHover  = kDefaultAccentHover;
        kAccentActive = kDefaultAccentActive;
        kHighlight       = kDefaultHighlight;
        kHighlightHover  = kDefaultHighlightHover;
        kHighlightActive = kDefaultHighlightActive;
        kPanelBorder      = kDefaultPanelBorder;
        Apply(); // rebuilds every ImGuiStyle.Colors[] entry from the reset accent
    }

    // -----------------------------------------------------------------------
    // Named themes
    // -----------------------------------------------------------------------

    // Started from the [ThemeColors] block actually shipped in a live
    // install's modloader.ini (translucent black panels, thin light-gray
    // rules, off-white text, orange accent), then split what was a single
    // accent triplet into three roles the game itself keeps visually
    // distinct: orange for values, cyan for hover/selected, white for panel
    // borders -- see the in-game pause menu's selected item.
    struct BuiltinColorEntry { const char* name; ImVec4 value; };
    static const BuiltinColorEntry kStarRupturePalette[] =
    {
        // Values (and their equivalents in the un-named ImGuiCol_* entries
        // below): orange, unchanged from the first Star Rupture palette.
        {"AccentBase", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"AccentHover", ImVec4(1.000000f, 0.682000f, 0.271000f, 1.000000f)},
        {"AccentActive", ImVec4(0.851000f, 0.471000f, 0.059000f, 1.000000f)},
        // Hover/selected: cyan, matching the in-game pause menu's selected
        // item color -- distinct from the orange values above, which the
        // single accent triplet used to force onto both meanings at once.
        {"Highlight", ImVec4(0.400000f, 0.860000f, 0.900000f, 1.000000f)},
        {"HighlightHover", ImVec4(0.550000f, 0.920000f, 0.950000f, 1.000000f)},
        {"HighlightActive", ImVec4(0.300000f, 0.740000f, 0.800000f, 1.000000f)},
        // Structure: white/near-white, matching the game's own panel borders.
        {"PanelBorder", ImVec4(0.900000f, 0.900000f, 0.900000f, 0.900000f)},
        {"Text", ImVec4(0.910000f, 0.910000f, 0.900000f, 1.000000f)},
        {"TextDisabled", ImVec4(0.550000f, 0.560000f, 0.570000f, 1.000000f)},
        {"WindowBg", ImVec4(0.086000f, 0.086000f, 0.090000f, 0.920000f)},
        {"ChildBg", ImVec4(0.110000f, 0.112000f, 0.118000f, 0.850000f)},
        {"PopupBg", ImVec4(0.078000f, 0.078000f, 0.082000f, 0.970000f)},
        {"Border", ImVec4(0.860000f, 0.870000f, 0.880000f, 0.850000f)},
        {"BorderShadow", ImVec4(0.000000f, 0.000000f, 0.000000f, 0.000000f)},
        {"FrameBg", ImVec4(0.165000f, 0.169000f, 0.176000f, 1.000000f)},
        {"FrameBgHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.120000f)},
        {"FrameBgActive", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.220000f)},
        {"TitleBg", ImVec4(0.060000f, 0.060000f, 0.065000f, 1.000000f)},
        {"TitleBgActive", ImVec4(0.060000f, 0.060000f, 0.065000f, 1.000000f)},
        {"TitleBgCollapsed", ImVec4(0.060000f, 0.060000f, 0.065000f, 0.800000f)},
        {"MenuBarBg", ImVec4(0.100000f, 0.100000f, 0.105000f, 1.000000f)},
        {"ScrollbarBg", ImVec4(0.060000f, 0.060000f, 0.065000f, 0.600000f)},
        {"ScrollbarGrab", ImVec4(0.350000f, 0.360000f, 0.370000f, 1.000000f)},
        {"ScrollbarGrabHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 1.000000f)},
        {"ScrollbarGrabActive", ImVec4(0.300000f, 0.740000f, 0.800000f, 1.000000f)},
        {"CheckMark", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"CheckboxSelectedBg", ImVec4(0.969000f, 0.580000f, 0.114000f, 0.250000f)},
        {"SliderGrab", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"SliderGrabActive", ImVec4(1.000000f, 0.682000f, 0.271000f, 1.000000f)},
        {"Button", ImVec4(0.120000f, 0.120000f, 0.125000f, 1.000000f)},
        {"ButtonHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.280000f)},
        {"ButtonActive", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.420000f)},
        {"Header", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.150000f)},
        {"HeaderHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.250000f)},
        {"HeaderActive", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.380000f)},
        {"Separator", ImVec4(0.620000f, 0.630000f, 0.650000f, 0.550000f)},
        {"SeparatorHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.800000f)},
        {"SeparatorActive", ImVec4(0.400000f, 0.860000f, 0.900000f, 1.000000f)},
        {"ResizeGrip", ImVec4(0.620000f, 0.630000f, 0.650000f, 0.250000f)},
        {"ResizeGripHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.600000f)},
        {"ResizeGripActive", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.900000f)},
        {"InputTextCursor", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"TabHovered", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.350000f)},
        {"Tab", ImVec4(0.100000f, 0.100000f, 0.105000f, 1.000000f)},
        {"TabSelected", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.220000f)},
        {"TabSelectedOverline", ImVec4(0.400000f, 0.860000f, 0.900000f, 1.000000f)},
        {"TabDimmed", ImVec4(0.080000f, 0.080000f, 0.085000f, 1.000000f)},
        {"TabDimmedSelected", ImVec4(0.969000f, 0.580000f, 0.114000f, 0.150000f)},
        {"TabDimmedSelectedOverline", ImVec4(0.969000f, 0.580000f, 0.114000f, 0.500000f)},
        {"PlotLines", ImVec4(0.620000f, 0.630000f, 0.650000f, 1.000000f)},
        {"PlotLinesHovered", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"PlotHistogram", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"PlotHistogramHovered", ImVec4(1.000000f, 0.682000f, 0.271000f, 1.000000f)},
        {"TableHeaderBg", ImVec4(0.120000f, 0.120000f, 0.125000f, 1.000000f)},
        {"TableBorderStrong", ImVec4(0.620000f, 0.630000f, 0.650000f, 0.550000f)},
        {"TableBorderLight", ImVec4(0.300000f, 0.310000f, 0.320000f, 0.500000f)},
        {"TableRowBg", ImVec4(0.000000f, 0.000000f, 0.000000f, 0.000000f)},
        {"TableRowBgAlt", ImVec4(1.000000f, 1.000000f, 1.000000f, 0.025000f)},
        {"TextLink", ImVec4(0.400000f, 0.860000f, 0.900000f, 1.000000f)},
        {"TextSelectedBg", ImVec4(0.400000f, 0.860000f, 0.900000f, 0.300000f)},
        {"TreeLines", ImVec4(0.620000f, 0.630000f, 0.650000f, 0.500000f)},
        {"DragDropTarget", ImVec4(0.969000f, 0.580000f, 0.114000f, 1.000000f)},
        {"DragDropTargetBg", ImVec4(0.969000f, 0.580000f, 0.114000f, 0.100000f)},
        {"UnsavedMarker", ImVec4(0.910000f, 0.910000f, 0.900000f, 1.000000f)},
        {"NavCursor", ImVec4(0.400000f, 0.860000f, 0.900000f, 1.000000f)},
        {"NavWindowingHighlight", ImVec4(1.000000f, 1.000000f, 1.000000f, 0.700000f)},
        {"NavWindowingDimBg", ImVec4(0.800000f, 0.800000f, 0.800000f, 0.200000f)},
        {"ModalWindowDimBg", ImVec4(0.000000f, 0.000000f, 0.000000f, 0.600000f)},
    };

    bool IsBuiltinTheme(const char* name)
    {
        return name && (strcmp(name, "Default") == 0 || strcmp(name, "Star Rupture") == 0);
    }

    // ModLoader\Themes\, creating it on first use -- CreateDirectoryW is a
    // no-op if it already exists, same as GetModLoaderDir() itself relies on.
    static std::wstring GetThemesDir()
    {
        std::wstring dir = GetModLoaderDir() + L"Themes\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    static std::wstring GetUserThemePath(const char* name)
    {
        return GetThemesDir() + ToWide(name) + L".ini";
    }

    int GetAvailableThemes(char outNames[][64], int maxCount)
    {
        int n = 0;
        if (n < maxCount) strncpy_s(outNames[n++], 64, "Default", _TRUNCATE);
        if (n < maxCount) strncpy_s(outNames[n++], 64, "Star Rupture", _TRUNCATE);

        const std::wstring pattern = GetThemesDir() + L"*.ini";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (n >= maxCount) break;

                wchar_t base[64] = {};
                wcsncpy_s(base, fd.cFileName, _TRUNCATE);
                wchar_t* dot = wcsrchr(base, L'.');
                if (dot) *dot = L'\0';
                if (base[0] == L'\0') continue;

                char narrow[64] = {};
                snprintf(narrow, sizeof(narrow), "%ls", base);
                strncpy_s(outNames[n++], 64, narrow, _TRUNCATE);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return n;
    }

    void ApplyTheme(const char* name)
    {
        if (!name || !name[0]) name = "Default";

        // Baseline every switch through ResetColors() -- Default's colors --
        // first, so a user theme that only overrides a handful of keys (or
        // a builtin request that matches neither name below, e.g. a stale
        // Theme= from a build with a different built-in list) still ends up
        // fully and predictably colored rather than a mix of two themes.
        ResetColors();

        if (strcmp(name, "Default") == 0)
            return;

        // Sentinel so a theme that sets its own accent but not Highlight/
        // PanelBorder still gets values that match *that* theme's accent --
        // not left at Default's from the ResetColors() baseline above, which
        // is only correct for Default itself. Alpha 0 is never a legitimate
        // opaque UI color, so "still zero after the theme below applied"
        // reliably means it didn't set one.
        kHighlight = kHighlightHover = kHighlightActive = kPanelBorder = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

        if (strcmp(name, "Star Rupture") == 0)
        {
            for (const auto& e : kStarRupturePalette)
                ApplyColorByName(e.name, e.value);
        }
        else
        {
            LoadColors(GetUserThemePath(name).c_str());
        }

        if (kHighlight.w       == 0.0f) kHighlight       = kAccent;
        if (kHighlightHover.w  == 0.0f) kHighlightHover  = kAccentHover;
        if (kHighlightActive.w == 0.0f) kHighlightActive = kAccentActive;
        if (kPanelBorder.w     == 0.0f) kPanelBorder      = kAccent;
    }

    void StartupLoadTheme(const wchar_t* iniPath)
    {
        const char* theme = UI::GlobalSettings::GetTheme();

        if (theme && theme[0])
        {
            ApplyTheme(theme);
            return;
        }

        // No [UI] Theme= key. If this install still has a legacy single
        // [ThemeColors] block from before named themes existed, migrate it
        // to a user theme called "Custom" instead of silently discarding it.
        wchar_t probe[8] = {};
        GetPrivateProfileStringW(L"ThemeColors", L"AccentBase", L"", probe, ARRAYSIZE(probe), iniPath);
        if (probe[0])
        {
            ResetColors();
            LoadColors(iniPath);              // apply the legacy block to the live style
            SaveColors(GetUserThemePath("Custom").c_str()); // ...and copy it into a real theme file
            UI::GlobalSettings::SetTheme("Custom");
            return;
        }

        ApplyTheme("Default");
    }

    bool SaveUserTheme(const char* name)
    {
        if (!name || !name[0] || IsBuiltinTheme(name)) return false;
        SaveColors(GetUserThemePath(name).c_str());
        return true;
    }

    bool DeleteUserTheme(const char* name)
    {
        if (!name || !name[0] || IsBuiltinTheme(name)) return false;
        return DeleteFileW(GetUserThemePath(name).c_str()) != 0;
    }

    void Apply()
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Square, angular look -- the chamfered-corner shape is drawn separately
        // via DrawChamferedBorder() since ImGuiStyle rounding is uniform-only.
        style.WindowRounding    = 0.0f;
        style.ChildRounding     = 0.0f;
        style.PopupRounding     = 0.0f;
        style.FrameRounding     = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding      = 0.0f;
        style.TabRounding       = 0.0f;

        style.WindowPadding = ImVec2(12.0f, 12.0f);
        style.FramePadding  = ImVec2(8.0f, 5.0f);
        style.ItemSpacing   = ImVec2(8.0f, 6.0f);
        style.CellPadding   = ImVec2(10.0f, 8.0f);
        style.ScrollbarSize = 14.0f;

        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize  = 1.0f;

        ImVec4* c = style.Colors;

        c[ImGuiCol_WindowBg]  = ImVec4(0.10f, 0.13f, 0.14f, 0.97f);
        c[ImGuiCol_ChildBg]   = ImVec4(0.12f, 0.155f, 0.165f, 1.00f);
        c[ImGuiCol_PopupBg]   = ImVec4(0.11f, 0.14f, 0.15f, 0.98f);

        c[ImGuiCol_Border]    = ImVec4(0.23f, 0.29f, 0.31f, 0.80f);
        c[ImGuiCol_Separator] = ImVec4(0.20f, 0.26f, 0.27f, 0.70f);

        c[ImGuiCol_Text]         = ImVec4(0.90f, 0.94f, 0.95f, 1.00f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.56f, 0.58f, 1.00f);

        c[ImGuiCol_FrameBg]        = ImVec4(0.14f, 0.18f, 0.19f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.18f);
        c[ImGuiCol_FrameBgActive]  = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);

        c[ImGuiCol_TitleBg]       = ImVec4(0.09f, 0.115f, 0.125f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.115f, 0.125f, 1.00f);

        c[ImGuiCol_Header]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.16f);
        c[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.28f);
        c[ImGuiCol_HeaderActive]  = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.40f);

        c[ImGuiCol_Button]        = ImVec4(0.16f, 0.20f, 0.21f, 1.00f);
        c[ImGuiCol_ButtonHovered] = kAccentHover;
        c[ImGuiCol_ButtonActive]  = kAccentActive;

        c[ImGuiCol_SliderGrab]       = kAccent;
        c[ImGuiCol_SliderGrabActive] = kAccentHover;
        c[ImGuiCol_CheckMark]        = kAccent;

        c[ImGuiCol_Tab]                = ImVec4(0.13f, 0.165f, 0.175f, 1.00f);
        c[ImGuiCol_TabHovered]         = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
        c[ImGuiCol_TabSelected]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f);
        c[ImGuiCol_TabSelectedOverline]   = kAccent;
        c[ImGuiCol_TabDimmed]           = ImVec4(0.11f, 0.14f, 0.15f, 1.00f);
        c[ImGuiCol_TabDimmedSelected]    = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.18f);

        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.09f, 0.115f, 0.125f, 1.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.20f, 0.26f, 0.27f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = kAccent;
        c[ImGuiCol_ScrollbarGrabActive]  = kAccentHover;

        c[ImGuiCol_TableHeaderBg]     = ImVec4(0.13f, 0.165f, 0.175f, 1.00f);
        c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.26f, 0.27f, 1.00f);
        c[ImGuiCol_TableBorderLight]  = ImVec4(0.16f, 0.20f, 0.21f, 1.00f);
    }

    void DrawChamferedBorder(const ImVec2& min, const ImVec2& max,
                              ImU32 color, float chamfer, float thickness)
    {
        // 6-point outline, chamfer cut at top-left and bottom-right only --
        // matches the asymmetric notch seen on the HUD panels (inventory,
        // player status), not a uniformly rounded/chamfered rect.
        ImVec2 pts[6] =
        {
            ImVec2(min.x + chamfer, min.y),
            ImVec2(max.x, min.y),
            ImVec2(max.x, max.y - chamfer),
            ImVec2(max.x - chamfer, max.y),
            ImVec2(min.x, max.y),
            ImVec2(min.x, min.y + chamfer),
        };
        ImGui::GetWindowDrawList()->AddPolyline(pts, 6, color, thickness, ImDrawFlags_Closed);
    }

    void DrawChamferedFill(const ImVec2& min, const ImVec2& max, ImU32 color, float chamfer)
    {
        ImVec2 pts[6] =
        {
            ImVec2(min.x + chamfer, min.y),
            ImVec2(max.x, min.y),
            ImVec2(max.x, max.y - chamfer),
            ImVec2(max.x - chamfer, max.y),
            ImVec2(min.x, max.y),
            ImVec2(min.x, min.y + chamfer),
        };
        ImGui::GetWindowDrawList()->AddConvexPolyFilled(pts, 6, color);
    }

    void DrawChamferedFillTopLeft(const ImVec2& min, const ImVec2& max, ImU32 color, float chamfer)
    {
        ImVec2 pts[5] =
        {
            ImVec2(min.x + chamfer, min.y),
            ImVec2(max.x, min.y),
            ImVec2(max.x, max.y),
            ImVec2(min.x, max.y),
            ImVec2(min.x, min.y + chamfer),
        };
        ImGui::GetWindowDrawList()->AddConvexPolyFilled(pts, 5, color);
    }

    bool DrawTitleBar(const char* title, const char* subtitle, bool showCloseButton)
    {
        // Much larger header with generous padding around the title, and a
        // bigger faux-bold rendering of the title text (drawn at 1.4x font
        // size, twice with a 1px offset -- ImGui has no separate bold
        // weight loaded, so this is the standard faux-bold trick).
        const float titleScale  = 1.4f;
        const float headerPadY  = 16.0f;
        const float headerHeight = ImGui::GetTextLineHeight() * titleScale + headerPadY * 2.0f;

        ImVec2 winPos  = ImGui::GetWindowPos();
        ImVec2 cursor  = ImGui::GetCursorScreenPos();
        float  width   = ImGui::GetWindowSize().x;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 headerMin = cursor;
        ImVec2 headerMax = ImVec2(winPos.x + width, cursor.y + headerHeight);
        DrawChamferedFillTopLeft(headerMin, headerMax, ImGui::GetColorU32(ImGuiCol_TitleBgActive));

        // Panel-border square indicator -- mirrors the HUD's small status
        // squares, part of the window's own frame/structure rather than a
        // value or a hover/selected state.
        const float sq = 10.0f;
        ImVec2 sqMin(headerMin.x + 20.0f, headerMin.y + (headerHeight - sq) * 0.5f);
        draw->AddRectFilled(sqMin, ImVec2(sqMin.x + sq, sqMin.y + sq), PanelBorderColor());

        ImFont* font     = ImGui::GetFont();
        float   titleSz  = ImGui::GetFontSize() * titleScale;
        ImVec2  titlePos(sqMin.x + sq + 16.0f, headerMin.y + (headerHeight - titleSz) * 0.5f);
        ImU32   titleCol = ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddText(font, titleSz, titlePos, titleCol, title);
        draw->AddText(font, titleSz, ImVec2(titlePos.x + 1.0f, titlePos.y), titleCol, title);

        if (subtitle && subtitle[0])
        {
            ImVec2 subSize = ImGui::CalcTextSize(subtitle);
            ImVec2 subPos(headerMax.x - subSize.x - 50.0f, headerMin.y + (headerHeight - ImGui::GetTextLineHeight()) * 0.5f);
            draw->AddText(subPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle);
        }

        bool stayOpen = true;
        if (showCloseButton)
        {
            ImGui::SetCursorScreenPos(ImVec2(headerMax.x - 14.0f - ImGui::GetFrameHeight(),
                                              headerMin.y + (headerHeight - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button("x##titlebar_close", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
                stayOpen = false;
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos(ImVec2(cursor.x, headerMax.y + 4.0f));
        return stayOpen;
    }

    bool BeginChamferedWindow(const char* windowId, const char* displayTitle, bool* open,
                               const char* subtitle, ImGuiWindowFlags extraFlags, bool showCloseButton)
    {
        // Native WindowBg/border are square rects -- suppressed so the
        // chamfered corner drawn below is genuinely transparent rather than
        // an outline sitting on top of an opaque square corner.
        ImU32 windowBgColor = ImGui::GetColorU32(ImGuiCol_WindowBg);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        bool isOpen = ImGui::Begin(windowId, open, ImGuiWindowFlags_NoTitleBar | extraFlags);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        if (!isOpen)
        {
            ImGui::End();
            return false;
        }

        DrawChamferedFill(ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                   ImGui::GetWindowPos().y + ImGui::GetWindowSize().y),
            windowBgColor);

        if (!DrawTitleBar(displayTitle, subtitle, showCloseButton) && open)
            *open = false;

        return true;
    }

    void EndChamferedWindow()
    {
        DrawChamferedBorder(ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                   ImGui::GetWindowPos().y + ImGui::GetWindowSize().y),
            PanelBorderColor());
        ImGui::End();
    }

    // Point size the tab label is drawn at. Deliberately below body text so a
    // one-word caption fits a 64px cell at the default font scale without
    // competing with the icon for attention.
    static float IconTabLabelFontSize()
    {
        return ImGui::GetFontSize() * 0.80f;
    }

    static float IconTabCellHeight(float size, bool hasLabels)
    {
        if (!hasLabels)
            return size;

        // The icon keeps a fixed square area and the caption sits below it, so
        // adding labels grows the cell rather than shrinking the glyph.
        return size * 0.86f + IconTabLabelFontSize() + 6.0f;
    }

    // Shared by IconTabBar and the public IconTabBarWidth so they can never
    // disagree about how wide a cell ends up. `size` was a fixed 64px used
    // as both the cell width AND the label's own clip rect -- fine at the
    // font scale it was measured at, but a caption clips the moment the UI
    // text size (FontScaleMain, user-adjustable in Settings) makes the
    // label's own CalcTextSizeA wider than that 64px, which every caption
    // eventually does as the scale climbs, not just an unusually long one.
    // Now the cell simply widens to fit -- never narrower than `size`
    // (the icon's own square area is untouched), only ever wider.
    static float IconTabCellWidth(const char* const* labels, int count, float size)
    {
        if (!labels)
            return size;

        ImFont*     font    = ImGui::GetFont();
        const float labelSz = IconTabLabelFontSize();
        float       cellW   = size;
        for (int i = 0; i < count; ++i)
            if (labels[i])
            {
                const float lw = font->CalcTextSizeA(labelSz, FLT_MAX, 0.0f, labels[i]).x;
                if (lw > cellW) cellW = lw;
            }
        return cellW;
    }

    float IconTabBarWidth(const char* const* labels, int count, float size)
    {
        return IconTabCellWidth(labels, count, size);
    }

    int IconTabBar(const char* const* icons, int count, int active, float size, bool vertical,
                   const char* const* labels, const char* const* tooltips)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        int newActive = active;
        ImVec2 startPos = ImGui::GetCursorScreenPos();

        const bool  hasLabels = (labels != nullptr);
        const float labelSz   = IconTabLabelFontSize();
        const float cellW     = IconTabCellWidth(labels, count, size);
        const float cellH     = IconTabCellHeight(size, hasLabels);
        const float iconAreaH = hasLabels ? size * 0.86f : size;

        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);
            ImVec2 boxMin = ImGui::GetCursorScreenPos();
            ImVec2 boxMax(boxMin.x + cellW, boxMin.y + cellH);

            ImGui::InvisibleButton("##icontab", ImVec2(cellW, cellH));
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked())
                newActive = i;

            // Prefer the longer description when one was supplied -- the label
            // under the icon is already on screen, so repeating it on hover
            // says nothing the user cannot see.
            const char* tip = (tooltips && tooltips[i]) ? tooltips[i]
                            : (labels && labels[i])     ? labels[i]
                            : nullptr;
            if (hovered && tip)
                ImGui::SetTooltip("%s", tip);

            // Active/hover highlight: inset rounded-rect rather than a sharp
            // square filling the whole cell, plus a thin divider line below
            // each cell (skipped after the last one).
            const float highlightInset = 6.0f;
            const float highlightRound = 8.0f;
            ImVec2 hlMin(boxMin.x + highlightInset, boxMin.y + highlightInset * 0.5f);
            ImVec2 hlMax(boxMax.x - highlightInset, boxMax.y - highlightInset * 0.5f);

            bool isActive = (i == active);
            if (isActive)
                draw->AddRectFilled(hlMin, hlMax, ImGui::GetColorU32(HighlightColorVec4(0.22f)), highlightRound);
            else if (hovered)
                draw->AddRectFilled(hlMin, hlMax, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f)), highlightRound);

            if (vertical && i + 1 < count)
                draw->AddLine(ImVec2(boxMin.x + 10.0f, boxMax.y), ImVec2(boxMax.x - 10.0f, boxMax.y),
                               ImGui::GetColorU32(ImGuiCol_Separator));

            // Render the glyph noticeably larger than body text so it reads
            // as an icon rather than a small character inside the box.
            // Centered on the glyph's actual visual bounding box (not its
            // advance-width box, which CalcTextSizeA returns) -- icon fonts
            // commonly have asymmetric left/right bearing that throws off
            // advance-based centering.
            ImFont* font   = ImGui::GetFont();
            float   iconSz = size * 0.45f;

            // ImFontBaked (not ImFont) owns FindGlyph() in this ImGui
            // version -- glyphs are baked per-size, so X0/Y0/X1/Y1 here are
            // already in pixels at iconSz with no extra scale needed.
            ImFontBaked* baked = font->GetFontBaked(iconSz);
            unsigned int codepoint = 0;
            ImTextCharFromUtf8(&codepoint, icons[i], nullptr);
            const ImFontGlyph* glyph = baked ? baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) : nullptr;

            ImVec2 textPos;
            if (glyph)
            {
                float gw = glyph->X1 - glyph->X0;
                float gh = glyph->Y1 - glyph->Y0;
                textPos = ImVec2(boxMin.x + (cellW - gw) * 0.5f - glyph->X0,
                                  boxMin.y + (iconAreaH - gh) * 0.5f - glyph->Y0);
            }
            else
            {
                ImVec2 textSize = font->CalcTextSizeA(iconSz, FLT_MAX, 0.0f, icons[i]);
                textPos = ImVec2(boxMin.x + (cellW - textSize.x) * 0.5f,
                                  boxMin.y + (iconAreaH - textSize.y) * 0.5f);
            }

            ImVec4 iconColV = isActive ? HighlightColorVec4(1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text];
            iconColV.w = isActive ? 1.0f : 0.55f; // "slightly opaque" when inactive
            draw->AddText(font, iconSz, textPos, ImGui::GetColorU32(iconColV), icons[i]);

            // Caption under the icon. Clipped to the cell rather than allowed
            // to bleed into the neighbouring column -- cellW already fits
            // this cell's own label in full (see IconTabCellWidth), so this
            // only ever clips a descender/accent riding slightly outside its
            // own glyph box, not the caption itself.
            if (hasLabels && labels[i])
            {
                ImVec2 lblSize = font->CalcTextSizeA(labelSz, FLT_MAX, 0.0f, labels[i]);
                ImVec2 lblPos(boxMin.x + (cellW - lblSize.x) * 0.5f,
                              boxMin.y + iconAreaH + 2.0f);

                ImVec4 lblColV = isActive ? HighlightColorVec4(1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text];
                lblColV.w = isActive ? 1.0f : 0.55f;

                draw->PushClipRect(ImVec2(boxMin.x, boxMin.y), ImVec2(boxMax.x, boxMax.y), true);
                draw->AddText(font, labelSz, lblPos, ImGui::GetColorU32(lblColV), labels[i]);
                draw->PopClipRect();
            }

            ImGui::PopID();
            if (vertical)
                ImGui::SetCursorScreenPos(ImVec2(startPos.x, boxMin.y + cellH));
            else
                ImGui::SameLine();
        }

        if (vertical)
            ImGui::SetCursorScreenPos(ImVec2(startPos.x + cellW, startPos.y));
        else
            ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + cellH));

        return newActive;
    }

    ImVec2 ToggleSwitchSize()
    {
        const float frameH = ImGui::GetFrameHeight();
        return ImVec2(frameH * 0.72f * 1.8f, frameH);
    }

    bool ToggleSwitch(const char* label, bool* v)
    {
        // The reserved layout size is the full frame height, same as a
        // button/slider/input box, so a toggle sitting next to one of those
        // in a row lines up with it without the caller having to compensate.
        // The drawn track stays smaller than that (0.72x) for its own look;
        // padY centers it within the reserved height.
        const ImVec2 sz     = ToggleSwitchSize();
        const float  frameH = sz.y;
        const float  width  = sz.x;
        const float  height = frameH * 0.72f;
        const float  notch  = height * 0.3f;
        const float  padY   = (frameH - height) * 0.5f;

        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(label, ImVec2(width, frameH));
        bool changed = false;
        if (ImGui::IsItemClicked())
        {
            *v = !*v;
            changed = true;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 trackMin = ImVec2(pos.x, pos.y + padY);
        ImVec2 trackMax = ImVec2(pos.x + width, pos.y + padY + height);

        ImU32 trackColor = *v
            ? ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f))
            : ImGui::GetColorU32(ImVec4(0.20f, 0.26f, 0.27f, 1.00f));

        // Notched track: chamfer cut at top-left corner only, knob fills the
        // opposite (on) side -- kept simple/un-animated for this pass.
        ImVec2 trackPts[5] =
        {
            ImVec2(trackMin.x + notch, trackMin.y),
            ImVec2(trackMax.x, trackMin.y),
            ImVec2(trackMax.x, trackMax.y),
            ImVec2(trackMin.x, trackMax.y),
            ImVec2(trackMin.x, trackMin.y + notch),
        };
        draw->AddConvexPolyFilled(trackPts, 5, trackColor);
        draw->AddPolyline(trackPts, 5, AccentColor(), 1.0f, ImDrawFlags_Closed);

        const float knobMargin = height * 0.16f;
        const float knobSize   = height - knobMargin * 2.0f;
        float knobX = *v
            ? trackMax.x - knobMargin - knobSize
            : trackMin.x + knobMargin;
        ImVec2 knobMin(knobX, trackMin.y + knobMargin);
        ImVec2 knobMax(knobX + knobSize, trackMax.y - knobMargin);
        draw->AddRectFilled(knobMin, knobMax, *v ? AccentColor() : ImGui::GetColorU32(ImGuiCol_TextDisabled));

        // ImGui convention: "##" marks the start of an ID-only suffix that is
        // never rendered (mirrors ImGui::FindRenderedTextEnd used internally
        // by Checkbox/Button etc.). Render nothing if the label has no text
        // before "##", same as a bare checkbox glyph with an empty label.
        const char* hash = strstr(label, "##");
        if (hash != label)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding(); // match the reserved frameH height above
            ImGui::TextUnformatted(label, hash);
        }

        return changed;
    }

    bool IconButton(const char* icon, const char* id, float size)
    {
        if (size <= 0.0f)
            size = ImGui::GetFrameHeight();

        const ImVec2 pos = ImGui::GetCursorScreenPos();

        const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();

        // Theme-driven, not hardcoded -- same two colors any hyperlink-style
        // text in this UI already uses, so a theme that changes TextLink
        // (e.g. the cyan "hover/selected" accent) restyles this too.
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImU32 col = ImGui::GetColorU32((active || hovered) ? style.Colors[ImGuiCol_TextLink]
                                                                   : style.Colors[ImGuiCol_TextDisabled]);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 textSz = ImGui::CalcTextSize(icon);
        draw->AddText(ImVec2(pos.x + (size - textSz.x) * 0.5f, pos.y + (size - textSz.y) * 0.5f), col, icon);

        return pressed;
    }

    namespace Icons
    {
        // UTF-8 encodings of Material Icons Regular codepoints (Apache 2.0),
        // embedded as an RCDATA resource and merged into the atlas in
        // imgui_backend.cpp's RebuildFontAtlas().
        const char* Plugins  = "\xEE\xA1\xBB"; // extension  U+E87B
        const char* Config   = "\xEE\x90\xA9"; // tune       U+E429
        const char* Settings = "\xEE\xA2\xB8"; // settings   U+E8B8
        const char* Logging  = "\xEE\xA3\x92"; // subject    U+E8D2
        const char* Theme    = "\xEE\x90\x8A"; // palette    U+E40A
        const char* About    = "\xEE\xA2\x8E"; // info       U+E88E
        const char* Reset    = "\xEE\x81\x82"; // replay     U+E042
    }
}

#endif // MODLOADER_CLIENT_BUILD
