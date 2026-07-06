#include "pch.h"
#include "theme.h"

#ifdef MODLOADER_CLIENT_BUILD

#include <cstring>
#include <cstdio>
#include <cfloat>
#include <unordered_map>

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

    void SaveColors(const wchar_t* iniPath)
    {
        if (!iniPath || !iniPath[0]) return;

        ImGuiStyle& style = ImGui::GetStyle();
        wchar_t valBuf[64];
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const char* name = ImGui::GetStyleColorName(i);
            if (!name) continue;
            wchar_t wname[64];
            swprintf_s(wname, L"%S", name);
            FormatColor(valBuf, ARRAYSIZE(valBuf), style.Colors[i]);
            WritePrivateProfileStringW(L"ThemeColors", wname, valBuf, iniPath);
        }

        FormatColor(valBuf, ARRAYSIZE(valBuf), kAccent);
        WritePrivateProfileStringW(L"ThemeColors", L"AccentBase", valBuf, iniPath);
        FormatColor(valBuf, ARRAYSIZE(valBuf), kAccentHover);
        WritePrivateProfileStringW(L"ThemeColors", L"AccentHover", valBuf, iniPath);
        FormatColor(valBuf, ARRAYSIZE(valBuf), kAccentActive);
        WritePrivateProfileStringW(L"ThemeColors", L"AccentActive", valBuf, iniPath);
    }

    void LoadColors(const wchar_t* iniPath)
    {
        if (!iniPath || !iniPath[0]) return;

        ImGuiStyle& style = ImGui::GetStyle();
        wchar_t valBuf[64];
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const char* name = ImGui::GetStyleColorName(i);
            if (!name) continue;
            wchar_t wname[64];
            swprintf_s(wname, L"%S", name);
            GetPrivateProfileStringW(L"ThemeColors", wname, L"", valBuf, ARRAYSIZE(valBuf), iniPath);
            if (!valBuf[0]) continue;
            ImVec4 c;
            if (ParseColor(valBuf, c)) style.Colors[i] = c;
        }

        GetPrivateProfileStringW(L"ThemeColors", L"AccentBase", L"", valBuf, ARRAYSIZE(valBuf), iniPath);
        if (valBuf[0]) ParseColor(valBuf, kAccent);
        GetPrivateProfileStringW(L"ThemeColors", L"AccentHover", L"", valBuf, ARRAYSIZE(valBuf), iniPath);
        if (valBuf[0]) ParseColor(valBuf, kAccentHover);
        GetPrivateProfileStringW(L"ThemeColors", L"AccentActive", L"", valBuf, ARRAYSIZE(valBuf), iniPath);
        if (valBuf[0]) ParseColor(valBuf, kAccentActive);
    }

    void ResetColors()
    {
        kAccent       = kDefaultAccent;
        kAccentHover  = kDefaultAccentHover;
        kAccentActive = kDefaultAccentActive;
        Apply(); // rebuilds every ImGuiStyle.Colors[] entry from the reset accent
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
        ImGui::GetWindowDrawList()->AddPolyline(pts, 6, color, ImDrawFlags_Closed, thickness);
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

        // Accent square indicator -- mirrors the HUD's small status squares.
        const float sq = 10.0f;
        ImVec2 sqMin(headerMin.x + 20.0f, headerMin.y + (headerHeight - sq) * 0.5f);
        draw->AddRectFilled(sqMin, ImVec2(sqMin.x + sq, sqMin.y + sq), AccentColor());

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
            AccentColor());
        ImGui::End();
    }

    int IconTabBar(const char* const* icons, int count, int active, float size, bool vertical,
                   const char* const* labels)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        int newActive = active;
        ImVec2 startPos = ImGui::GetCursorScreenPos();

        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);
            ImVec2 boxMin = ImGui::GetCursorScreenPos();
            ImVec2 boxMax(boxMin.x + size, boxMin.y + size);

            ImGui::InvisibleButton("##icontab", ImVec2(size, size));
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked())
                newActive = i;

            if (hovered && labels && labels[i])
                ImGui::SetTooltip("%s", labels[i]);

            // Active/hover highlight: inset rounded-rect rather than a sharp
            // square filling the whole cell, plus a thin divider line below
            // each cell (skipped after the last one).
            const float highlightInset = 6.0f;
            const float highlightRound = 8.0f;
            ImVec2 hlMin(boxMin.x + highlightInset, boxMin.y + highlightInset);
            ImVec2 hlMax(boxMax.x - highlightInset, boxMax.y - highlightInset);

            bool isActive = (i == active);
            if (isActive)
                draw->AddRectFilled(hlMin, hlMax, ImGui::GetColorU32(AccentColorVec4(0.22f)), highlightRound);
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
                textPos = ImVec2(boxMin.x + (size - gw) * 0.5f - glyph->X0,
                                  boxMin.y + (size - gh) * 0.5f - glyph->Y0);
            }
            else
            {
                ImVec2 textSize = font->CalcTextSizeA(iconSz, FLT_MAX, 0.0f, icons[i]);
                textPos = ImVec2(boxMin.x + (size - textSize.x) * 0.5f, boxMin.y + (size - textSize.y) * 0.5f);
            }

            ImVec4 iconColV = isActive ? AccentColorVec4(1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text];
            iconColV.w = isActive ? 1.0f : 0.55f; // "slightly opaque" when inactive
            draw->AddText(font, iconSz, textPos, ImGui::GetColorU32(iconColV), icons[i]);

            ImGui::PopID();
            if (vertical)
                ImGui::SetCursorScreenPos(ImVec2(startPos.x, boxMin.y + size));
            else
                ImGui::SameLine();
        }

        if (vertical)
            ImGui::SetCursorScreenPos(ImVec2(startPos.x + size, startPos.y));
        else
            ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + size));

        return newActive;
    }

    bool ToggleSwitch(const char* label, bool* v)
    {
        const float height = ImGui::GetFrameHeight() * 0.72f;
        const float width  = height * 1.8f;
        const float notch   = height * 0.3f;

        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(label, ImVec2(width, height));
        bool changed = false;
        if (ImGui::IsItemClicked())
        {
            *v = !*v;
            changed = true;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 trackMin = pos;
        ImVec2 trackMax = ImVec2(pos.x + width, pos.y + height);

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
        draw->AddPolyline(trackPts, 5, AccentColor(), ImDrawFlags_Closed, 1.0f);

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
            ImGui::TextUnformatted(label, hash);
        }

        return changed;
    }

    void MarqueeLabel(const char* id, const char* text, float width)
    {
        if (width <= 0.0f)
            width = ImGui::GetContentRegionAvail().x;

        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const float  lineH    = ImGui::GetTextLineHeight();
        const ImVec2 pos      = ImGui::GetCursorScreenPos();

        // Reserve the box so the cursor advances like a normal text item and
        // tables/SameLine() lay out correctly regardless of scroll state.
        ImGui::Dummy(ImVec2(width, lineH));

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 col  = ImGui::GetColorU32(ImGuiCol_Text);
        const float overflow = textSize.x - width;

        if (overflow <= 0.0f)
        {
            // Fits -- draw statically, no scroll state needed.
            draw->AddText(pos, col, text);
            return;
        }

        // Per-label scroll phase, keyed by a stable id. Phase runs 0..1 forward
        // (scroll left), holds at each end, then reverses -- a ping-pong so both
        // ends of the string are readable.
        static std::unordered_map<ImGuiID, float> s_phase;
        const ImGuiID key = ImGui::GetID(id);
        float& phase = s_phase[key];

        // Speed is in pixels/sec; convert to phase/sec over the overflow span so
        // long and short overflows scroll at a consistent visual rate.
        const float kPixelsPerSec = 30.0f;
        const float kPauseSecs    = 1.2f;
        const float span          = overflow;
        const float dt            = ImGui::GetIO().DeltaTime;

        // Encode pause into the phase range: [-pauseFrac, 0] left hold,
        // [1, 1+pauseFrac] right hold, wrapping via a triangle wave over a
        // period that includes both holds.
        const float scrollSecs = span / kPixelsPerSec;
        const float period     = 2.0f * (scrollSecs + kPauseSecs);
        phase += dt;
        if (phase >= period) phase -= period * (float)(int)(phase / period);

        float t = phase;
        float scroll; // 0 = fully left (start), 1 = fully right (end shown)
        if (t < kPauseSecs)                              scroll = 0.0f;                       // hold start
        else if (t < kPauseSecs + scrollSecs)            scroll = (t - kPauseSecs) / scrollSecs;
        else if (t < 2.0f * kPauseSecs + scrollSecs)     scroll = 1.0f;                       // hold end
        else                                             scroll = 1.0f - (t - (2.0f * kPauseSecs + scrollSecs)) / scrollSecs;

        const float offset = scroll * overflow;

        // Clip to the reserved box so the scrolled text does not bleed into
        // neighbouring columns/widgets.
        draw->PushClipRect(pos, ImVec2(pos.x + width, pos.y + lineH), true);
        draw->AddText(ImVec2(pos.x - offset, pos.y), col, text);
        draw->PopClipRect();
    }

    namespace Icons
    {
        // UTF-8 encodings of Material Icons Regular codepoints (Apache 2.0),
        // embedded as an RCDATA resource and merged into the atlas in
        // imgui_backend.cpp's RebuildFontAtlas().
        const char* Plugins  = "\xEE\xA1\xBB"; // extension  U+E87B
        const char* Config   = "\xEE\x90\xA9"; // tune       U+E429
        const char* Settings = "\xEE\xA2\xB8"; // settings   U+E8B8
        const char* Theme    = "\xEE\x90\x8A"; // palette    U+E40A
        const char* About    = "\xEE\xA2\x8E"; // info       U+E88E
    }
}

#endif // MODLOADER_CLIENT_BUILD
