#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <imgui.h>

// ---------------------------------------------------------------------------
// UI::Theme
//
// Applies the modloader's own window style: dark slate panels, cyan/teal
// accent, and angular (single-corner-chamfered) panel edges matching
// StarRupture's in-game HUD, instead of generic ImGui::StyleColorsDark().
//
// Apply() touches the global ImGuiStyle, so it affects every ImGui window
// drawn through the shared StarRupture-ImGui context -- including plugin
// panels -- even though only the modloader's own window is restyled with
// the custom widgets (ToggleSwitch, DrawChamferedBorder) in this pass.
// ---------------------------------------------------------------------------
namespace UI::Theme
{
    // Sets ImGuiStyle colors/spacing for the cyan/teal HUD-matched look.
    // Safe to call more than once (idempotent) -- call once per session,
    // after the ImGui context exists.
    void Apply();

    ImU32 AccentColor();
    ImU32 AccentColorHover();
    ImVec4 AccentColorVec4(float alpha = 1.0f);

    // Mutable accent triplet -- drives custom-drawn widgets that represent a
    // *value* (ToggleSwitch's on state, SliderGrab, CheckMark) rather than a
    // hover/selected state (see the Highlight triplet below for that).
    // Pointers are returned so the Theme tab can bind them straight to
    // ImGui::ColorEdit4 -- edits take effect immediately, no extra plumbing
    // needed.
    ImVec4* AccentBasePtr();
    ImVec4* AccentHoverPtr();
    ImVec4* AccentActivePtr();

    // Highlight triplet -- separate from the accent above, for
    // hover/selected custom-drawn state instead of a value: the active
    // sidebar tab's icon/label and its cell fill. Defaults to the matching
    // accent value (see ApplyTheme), so a theme that never sets these looks
    // exactly as it would have before they existed.
    ImU32 HighlightColor();
    ImU32 HighlightColorHover();
    ImVec4 HighlightColorVec4(float alpha = 1.0f);

    ImVec4* HighlightBasePtr();
    ImVec4* HighlightHoverPtr();
    ImVec4* HighlightActivePtr();

    // Panel border -- the chamfered window border and the title bar's
    // accent-square indicator: the window's own frame/structure, not a
    // value or a hover/selected state. Also defaults to the accent (see
    // ApplyTheme) when a theme doesn't set it.
    ImU32   PanelBorderColor();
    ImVec4* PanelBorderPtr();

    // Persist every ImGuiStyle.Colors[] entry plus the accent/highlight
    // triplets and the panel border to modloader.ini under [ThemeColors],
    // keyed by ImGui::GetStyleColorName() for the former.
    void SaveColors(const wchar_t* iniPath);

    // Overwrite ImGuiStyle.Colors[] and the accent/highlight triplets from
    // modloader.ini, for any keys present under [ThemeColors]. Call once at
    // startup, after Apply() has seeded the defaults -- entries absent from
    // the ini are left at whatever Apply() set them to.
    void LoadColors(const wchar_t* iniPath);

    // Reset the accent triplet to its built-in default and re-run Apply()
    // to rebuild every ImGuiStyle.Colors[] entry from it, discarding any
    // per-color overrides made in the Theme tab. Does not touch disk --
    // call SaveColors() afterward to persist the reset.
    void ResetColors();

    // -----------------------------------------------------------------------
    // Named themes
    //
    // "Default" (Apply()'s built-in cyan) and "Star Rupture" (a second
    // built-in, embedded at compile time from the palette actually shipped
    // in a live install's modloader.ini) always exist and are never written
    // to disk. Anything else is a user theme file at
    // ModLoader\Themes\<name>.ini, in the same [ThemeColors] format
    // Save/LoadColors already read and write.
    // -----------------------------------------------------------------------

    bool IsBuiltinTheme(const char* name);

    // Fills outNames (up to maxCount entries, each null-terminated within 64
    // bytes) with every theme available right now: the two built-ins first,
    // then one entry per ModLoader\Themes\*.ini file. Returns the count
    // written.
    int GetAvailableThemes(char outNames[][64], int maxCount);

    // Applies `name` to the live ImGuiStyle immediately -- Apply()/
    // ResetColors() first for a clean baseline, then the built-in Star
    // Rupture table or the named user theme file on top of it, so a user
    // theme only has to specify the keys it actually overrides. An unknown
    // user theme name (e.g. its file was deleted) just leaves the baseline
    // applied. Does not touch modloader.ini; pair with
    // GlobalSettings::SetTheme() to persist the switch.
    void ApplyTheme(const char* name);

    // Reads modloader.ini [UI] Theme= (via GlobalSettings, already loaded by
    // this point) and applies it. Migration: if that key is absent but
    // iniPath still has a legacy [ThemeColors] block from before named
    // themes existed, saves it as a new user theme called "Custom", points
    // [UI] Theme= at it, and applies it -- so nobody's colors disappear.
    // Call once at startup, in place of the old Apply()+LoadColors() pair,
    // once the ImGui context exists.
    void StartupLoadTheme(const wchar_t* iniPath);

    // Writes the live ImGuiStyle + accent triplet to
    // ModLoader\Themes\<name>.ini. Returns false and does nothing for a
    // builtin name -- Default and Star Rupture ship in the binary.
    bool SaveUserTheme(const char* name);

    // Deletes ModLoader\Themes\<name>.ini. Returns false and does nothing
    // for a builtin name. Does not change the active theme -- if `name` was
    // active, the caller is responsible for switching away first.
    bool DeleteUserTheme(const char* name);

    // Drop-in replacement for ImGui::Checkbox with a notched-corner sliding
    // toggle-switch look instead of a checkbox glyph. Same call contract:
    // returns true the frame the value changes.
    bool ToggleSwitch(const char* label, bool* v);

    // The layout footprint one ToggleSwitch reserves (x = width, y = the
    // full frame height it's centered within -- see ToggleSwitch's own
    // comment). Call this instead of re-deriving its internal size ratio
    // at a distant call site that needs to lay out space for a toggle
    // before drawing one, e.g. sizing a column to fit it.
    ImVec2 ToggleSwitchSize();

    // A borderless clickable icon glyph -- no button frame/box, just the
    // glyph itself over an InvisibleButton hit area. Colored by state
    // (ImGuiCol_TextDisabled at rest, ImGuiCol_TextLink on hover or while
    // held) rather than a hardcoded color, so any theme controls its look
    // the same way it controls every other color. `size` <= 0 uses
    // GetFrameHeight(). Returns true the frame it's clicked.
    bool IconButton(const char* icon, const char* id, float size = 0.0f);

    // Draws a single-diagonal-corner (chamfered) outline -- chamfer cut at
    // the top-left and bottom-right corners -- over the given rect, matching
    // the angular panel shape used throughout the StarRupture HUD. Call after
    // the content it outlines has been drawn (e.g. right before ImGui::End()).
    // Shared chamfer amount -- kept consistent between the window's
    // background fill, its outline, and the title-bar header strip so the
    // cut corner lines up across all three.
    inline constexpr float kWindowChamfer = 12.0f;

    void DrawChamferedBorder(const ImVec2& min, const ImVec2& max,
                              ImU32 color, float chamfer = kWindowChamfer,
                              float thickness = 1.5f);

    // Filled chamfered rect (top-left and bottom-right corners cut) --
    // use as the window's own background fill in place of ImGuiCol_WindowBg
    // so the chamfer cutout is genuinely transparent rather than just an
    // outline drawn over an opaque square corner.
    void DrawChamferedFill(const ImVec2& min, const ImVec2& max,
                            ImU32 color, float chamfer = kWindowChamfer);

    // Filled rect with only the top-left corner chamfered -- for the
    // title-bar header strip, whose top-left corner sits on the window's
    // outer (chamfered) corner but whose other corners are interior.
    void DrawChamferedFillTopLeft(const ImVec2& min, const ImVec2& max,
                                   ImU32 color, float chamfer = kWindowChamfer);

    // Draws a custom header strip (accent square + title + right-aligned
    // subtitle + close button) in place of ImGui's native title bar.
    // Call right after ImGui::Begin() on a window opened with
    // ImGuiWindowFlags_NoTitleBar; advances the cursor past the header.
    // Returns false if the close button was clicked this frame (caller
    // should clear its "is open" flag).
    // showCloseButton: pass false to omit the (x) close button -- e.g. for
    // plugin widget windows, which have no concept of being user-closed.
    bool DrawTitleBar(const char* title, const char* subtitle, bool showCloseButton = true);

    // Begins a window styled like the modloader's own window: transparent
    // native chrome, a chamfered background fill, and a custom title bar
    // (DrawTitleBar) in place of ImGui's title bar. Pairs with
    // EndChamferedWindow(); call that only if this returns true -- if it
    // returns false the window was collapsed/not visible and ImGui::End()
    // has already been called internally.
    //   windowId        -- passed to ImGui::Begin() as-is (may include a
    //                       "##" ID-hiding suffix)
    //   displayTitle    -- text shown in the title bar (drawn verbatim, no
    //                       "##" stripping)
    //   open            -- standard ImGui Begin "p_open"; set to false if
    //                       the title bar's close button is clicked
    //   subtitle        -- optional right-aligned subtitle, or nullptr
    //   extraFlags      -- OR'd into the window flags (NoTitleBar is always
    //                       set regardless of what's passed here)
    //   showCloseButton -- forwarded to DrawTitleBar()
    bool BeginChamferedWindow(const char* windowId, const char* displayTitle, bool* open,
                               const char* subtitle = nullptr, ImGuiWindowFlags extraFlags = 0,
                               bool showCloseButton = true);

    // Draws the chamfered border and calls ImGui::End(). Call only after a
    // BeginChamferedWindow() that returned true.
    void EndChamferedWindow();

    // Icon tab strip -- draws `count` cells at least `size` wide starting at
    // the current cursor, each showing icons[i] centered. The active cell
    // gets full opacity + an accent fill/border; inactive cells are dimmed.
    // Returns the new active index (== `active` if nothing was clicked this
    // frame).
    // vertical=false: cells laid out left-to-right; cursor drops to the
    //   next line below the row once done.
    // vertical=true: cells stacked top-to-bottom; cursor moves to the right
    //   of the column at the original starting Y, so the caller can place
    //   tab content directly beside it without a child window.
    // labels: optional, parallel array of `count` display names. Drawn UNDER
    //   the icon in a smaller font, centered in the cell -- an icon on its
    //   own does not tell a first-time user what the tab is, which is what
    //   this is for. Keep them to one short word; pass nullptr for an
    //   icon-only strip. Each cell widens past `size` (never narrower) to
    //   fit its own label in full at the current font scale, so raising the
    //   UI's text size doesn't clip a caption -- call IconTabBarWidth() with
    //   the same labels/count/size first if the caller needs to size a
    //   container around the strip before drawing it (e.g. a nav column's
    //   own child window).
    // tooltips: optional, parallel array of `count` longer descriptions shown
    //   on hover. Falls back to labels[i] when null, so passing labels alone
    //   keeps the old hover behaviour.
    int IconTabBar(const char* const* icons, int count, int active,
                    float size = 64.0f, bool vertical = false,
                    const char* const* labels = nullptr,
                    const char* const* tooltips = nullptr);

    // The width IconTabBar will actually use for each cell, given the same
    // labels/count/size -- `size` itself if every label already fits (or
    // labels is null), otherwise the widest label's own text width (at
    // IconTabBar's own label font size, a fraction of the current UI text
    // size) plus breathing room. Call this BEFORE IconTabBar when sizing a
    // container around it, so the container doesn't end up narrower than
    // what IconTabBar is about to draw.
    float IconTabBarWidth(const char* const* labels, int count, float size = 64.0f);

    // Icon glyph constants (Material Icons Regular, embedded resource --
    // see imgui_backend.cpp RebuildFontAtlas()).
    namespace Icons
    {
        extern const char* Plugins;
        extern const char* Config;
        extern const char* Settings;
        extern const char* Logging;
        extern const char* Theme;
        extern const char* About;
        extern const char* Reset; // replay -- reset buttons (config rows, Logging tab)
    }
}

#endif // MODLOADER_CLIENT_BUILD
