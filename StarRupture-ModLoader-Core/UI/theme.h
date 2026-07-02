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

    // Mutable accent triplet -- drives custom-drawn widgets (ToggleSwitch,
    // chamfered borders/fills, IconTabBar highlight, title-bar accent
    // square) that read these directly each frame rather than going through
    // ImGuiStyle.Colors. Pointers are returned so the Theme tab can bind
    // them straight to ImGui::ColorEdit4 -- edits take effect immediately,
    // no extra plumbing needed.
    ImVec4* AccentBasePtr();
    ImVec4* AccentHoverPtr();
    ImVec4* AccentActivePtr();

    // Persist every ImGuiStyle.Colors[] entry plus the accent triplet to
    // modloader.ini under [ThemeColors], keyed by ImGui::GetStyleColorName().
    void SaveColors(const wchar_t* iniPath);

    // Overwrite ImGuiStyle.Colors[] and the accent triplet from
    // modloader.ini, for any keys present under [ThemeColors]. Call once at
    // startup, after Apply() has seeded the defaults -- entries absent from
    // the ini are left at whatever Apply() set them to.
    void LoadColors(const wchar_t* iniPath);

    // Reset the accent triplet to its built-in default and re-run Apply()
    // to rebuild every ImGuiStyle.Colors[] entry from it, discarding any
    // per-color overrides made in the Theme tab. Does not touch disk --
    // call SaveColors() afterward to persist the reset.
    void ResetColors();

    // Drop-in replacement for ImGui::Checkbox with a notched-corner sliding
    // toggle-switch look instead of a checkbox glyph. Same call contract:
    // returns true the frame the value changes.
    bool ToggleSwitch(const char* label, bool* v);

    // Draws `text` clipped to `width` pixels on the current line. If it fits,
    // it is drawn statically like TextUnformatted. If it overflows, it scrolls
    // horizontally (pause at each end, then reverse) so the whole string can be
    // read without a tooltip. `id` must be stable across frames -- it keys the
    // per-label scroll state -- and is not rendered (pass e.g. "##desc_foo").
    // Advances the cursor by one line height and reserves `width` horizontally,
    // so it composes with SameLine()/tables like a normal text item.
    void MarqueeLabel(const char* id, const char* text, float width);

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

    // Square icon-only tab strip (no text) -- draws `count` size x size boxes
    // starting at the current cursor, each showing icons[i] centered. The
    // active box gets full opacity + an accent fill/border; inactive boxes
    // are dimmed. Returns the new active index (== `active` if nothing was
    // clicked this frame).
    // vertical=false: boxes laid out left-to-right; cursor drops to the
    //   next line below the row once done.
    // vertical=true: boxes stacked top-to-bottom; cursor moves to the right
    //   of the column at the original starting Y, so the caller can place
    //   tab content directly beside it without a child window.
    int IconTabBar(const char* const* icons, int count, int active,
                    float size = 64.0f, bool vertical = false);

    // Icon glyph constants (Material Icons Regular, embedded resource --
    // see imgui_backend.cpp RebuildFontAtlas()).
    namespace Icons
    {
        extern const char* Plugins;
        extern const char* Config;
        extern const char* Settings;
        extern const char* Theme;
        extern const char* About;
    }
}

#endif // MODLOADER_CLIENT_BUILD
