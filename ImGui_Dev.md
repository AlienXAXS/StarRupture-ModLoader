# ImGui Integration — Plugin Developer Guide

The modloader owns Dear ImGui entirely. Plugins do **not** link against ImGui directly — all
rendering is done through a function-pointer table (`IModLoaderImGui`) passed into your render
callback. This keeps the ImGui version and DX12 backend entirely under the modloader's control.

The UI system is **client-only**. On server or generic builds `hooks->UI` and `hooks->Input` are
both `nullptr`. Always null-check before use.

Icons: the modloader bundles the full Material Icons font, and because ImGui 1.92 bakes glyphs on
demand, **all 2188 of them are usable from a plugin with no setup at all** — see
[MaterialIcons.md](MaterialIcons.md) for the contact sheets and the codepoint/escape table.

---

## Availability

```ini
; modloader.ini — users can disable the overlay entirely
[UI]
Enabled=1       ; set to 0 to skip ImGui initialisation (no hooks, no rendering)
OpenKey=F2      ; key that opens/closes the ModLoader window
```

If `Enabled=0` your panel and widget callbacks will never fire and `hooks->UI` will still be
non-null, but `RegisterPanel` and `RegisterWidget` are no-ops because the backend never starts.

---

## Registering a Custom Panel

### 1. Write a render callback

```cpp
static void MyPanelRender(IModLoaderImGui* imgui)
{
    imgui->Text("Hello from MyPlugin!");
    imgui->Spacing();

    static bool toggle = false;
    if (imgui->Checkbox("Enable feature", &toggle))
    {
        // value changed — do something
    }

    static float speed = 1.0f;
    if (imgui->SliderFloat("Speed", &speed, 0.1f, 10.0f, "%.2f"))
    {
        // slider moved
    }
}
```

### 2. Declare a PluginPanelDesc

The descriptor and all strings it references must remain valid for as long as the panel is
registered (i.e. for the lifetime of the plugin is fine).

```cpp
static const PluginPanelDesc k_myPanel = {
    "My Plugin",          // button label shown in the ModLoader Config tab
    "My Plugin##panel",   // ImGui window title — must be globally unique
    MyPanelRender         // render callback
};
```

### 3. Register in PluginInit

`RegisterPanel` returns a `PanelHandle` — an opaque pointer that uniquely identifies your panel.
Store it; you will need it to open, close, or toggle the panel from a keybind.

```cpp
static IPluginHooks* g_hooks  = nullptr;
static PanelHandle   g_panel  = nullptr;   // returned by RegisterPanel

bool PluginInit(IPluginSelf* self)
{
    g_hooks = self->hooks;

    if (g_hooks->UI)
        g_panel = g_hooks->UI->RegisterPanel(&k_myPanel);
    // g_panel is null if the UI backend is disabled or registration failed

    return true;
}
```

### 4. Unregister in PluginShutdown

```cpp
void PluginShutdown()
{
    if (g_hooks && g_hooks->UI)
        g_hooks->UI->UnregisterPanel(g_panel);
    g_panel = nullptr;
}
```

Once registered a button labelled with `buttonLabel` appears in the **Config** tab of the
ModLoader window (under "Plugin Tools"). Clicking it opens your panel window; the render callback
is called every frame while the window is open.

---

## Opening a Panel Programmatically (e.g. on Keypress)

Panels can be opened or closed from anywhere — including keybind callbacks — using the
`PanelHandle` returned by `RegisterPanel`. Because the handle is opaque, a plugin can only affect
its own panels; there is no way to open or close another plugin's panel.

```cpp
static IPluginHooks* g_hooks = nullptr;
static PanelHandle   g_panel = nullptr;

static void OnToggleKey(EModKey, EModKeyEvent)
{
    if (!g_hooks || !g_hooks->UI || !g_panel) return;

    // Use SetPanelClose if the panel is already open, SetPanelOpen to show it.
    // Track state with a static bool so the same key acts as a toggle.
    static bool s_open = false;
    s_open = !s_open;
    if (s_open)
        g_hooks->UI->SetPanelOpen(g_panel);
    else
        g_hooks->UI->SetPanelClose(g_panel);
}

bool PluginInit(IPluginSelf* self)
{
    g_hooks = self->hooks;

    if (g_hooks->UI)
        g_panel = g_hooks->UI->RegisterPanel(&k_myPanel);

    if (g_hooks->Input)
        g_hooks->Input->RegisterKeybind(EModKey::Insert, EModKeyEvent::Pressed, OnToggleKey);

    return true;
}
```

`SetPanelOpen` / `SetPanelClose` are silently ignored if the handle is null (e.g. UI backend
disabled). The panel does **not** need to be visible in the ModLoader Config tab first —
`SetPanelOpen` is equivalent to the user clicking the button there.

---

## Registering an Always-On Widget

Widgets are ImGui windows that render **every frame** regardless of whether the ModLoader window
is open. Use them for persistent HUD overlays — status info, debug values, player-togglable
displays — anything that should stay on screen during normal gameplay.

Unlike panels (which only render while the user has them open via the Config tab), widget windows
are always visible once registered. Visibility can be toggled programmatically via
`SetWidgetVisible`.

### 1. Write a render callback

The signature is identical to a panel render callback.

```cpp
static void MyWidgetRender(IModLoaderImGui* imgui)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Enemies nearby: %d", g_nearbyCount);
    imgui->Text(buf);

    snprintf(buf, sizeof(buf), "Status: %s", g_status);
    imgui->TextColored(0.2f, 1.0f, 0.2f, 1.0f, buf);
}
```

### 2. Declare a PluginWidgetDesc

```cpp
static const PluginWidgetDesc k_myWidget = {
    "My Plugin HUD",   // ImGui window title — must be globally unique; also shown as the title bar
    MyWidgetRender     // render callback
};
```

### 3. Register in PluginInit

```cpp
static IPluginHooks* g_hooks  = nullptr;
static WidgetHandle  g_widget = nullptr;

bool PluginInit(IPluginSelf* self)
{
    g_hooks = self->hooks;

    if (g_hooks->UI)
        g_widget = g_hooks->UI->RegisterWidget(&k_myWidget);

    return true;
}
```

### 4. Unregister in PluginShutdown

```cpp
void PluginShutdown()
{
    if (g_hooks && g_hooks->UI)
        g_hooks->UI->UnregisterWidget(g_widget);
    g_widget = nullptr;
}
```

### 5. Toggling visibility

Widgets are visible by default. Call `SetWidgetVisible` at any time — from a keybind, a config
change callback, or anywhere else — to let players hide or show the widget:

```cpp
static bool s_widgetVisible = true;

static void OnToggleWidget(EModKey, EModKeyEvent)
{
    if (!g_hooks || !g_hooks->UI || !g_widget) return;
    s_widgetVisible = !s_widgetVisible;
    g_hooks->UI->SetWidgetVisible(g_widget, s_widgetVisible);
}

// In PluginInit, after registering the widget:
if (hooks->Input)
    hooks->Input->RegisterKeybind(EModKey::F6, EModKeyEvent::Pressed, OnToggleWidget);
```

The widget window position and size are saved to `modloader_imgui.ini` automatically, so the
player's chosen position persists between sessions.

### 6. Over the pause menu and other cursor screens (v71)

While the game's pause menu is open, or any other game screen is showing the mouse cursor (the
main menu, machine menus, the recipe station, the inventory), and no ModLoader window has the
cursor, the game shows its own cursor. Widgets are click-through there by default, so the menu buttons under them keep
working. To let players hover, click and drag a widget over the menu, opt in with
`PluginWindowFlags_PauseMenuMouseInputs` in its window hints:

```cpp
static const PluginWindowHints k_myHints = {
    0.0f, 0.0f,     // width, height: no size hint
    -1.0f, -1.0f,   // pos_x, pos_y: no placement
    0.0f, 0.0f,     // pivot
    0, 0,           // size_cond, pos_cond
    PluginWindowFlags_PauseMenuMouseInputs | PluginWindowFlags_AlwaysAutoResize
};

static const PluginWidgetDesc k_myWidget = {
    "My Plugin HUD",
    MyWidgetRender,
    &k_myHints
};
```

A widget without hints is auto-sized; one with hints is not unless it asks, hence `PluginWindowFlags_AlwaysAutoResize` (v74; before that, `1 << 6`).

The same holds over every other game screen that has the mouse cursor up: the loader reads
`bShowMouseCursor` on the local player controller once per game tick. A click on an opted-in
widget is kept from the menu; a click anywhere else reaches the menu. While
one of its popups is open, ImGui keeps the first click outside it to close the popup. The keyboard
stays with the game, so Escape still closes the menu, except that a focused text field keeps its
keys. The flag applies to the widget's own window:
a child window inside a widget that did not opt in (`BeginChild`, a scrolling table) is an ImGui
window of its own and still takes the mouse. While a ModLoader window has the cursor (F2, the
console), the flags change nothing: widgets take the mouse as they always have. An open plugin
panel or an `AcquireInputCapture` token does not take the cursor over these screens: the mouse stays
shared, so only the plugin's own windows take it and the rest of the screen keeps its clicks.
Mouse queries (`GetMousePos`, `IsMouseDown`, `IsMouseClicked`) report the menu cursor in every
widget, opted in or not; only hovering and clicking the window itself is gated.

`PluginWindowFlags_PauseMenuMouseInputs` takes the mouse everywhere the widget is drawn, including over
the menu's own buttons. A widget that should stay usable without covering them can opt in with
`PluginWindowFlags_MenuMouseWhereUncovered` (v75) instead: it takes the mouse wherever no widget of the
game's menu is under the cursor, and is click-through where one is, so the menu's buttons and panels
keep their clicks. The loader asks the game once per tick whether a widget on its menu layer (where the
map, the inventory, the build menu, the machine windows and the Escape menu open) is hovered, so the
answer can be a frame old: a click just after the cursor moves onto a menu button can still land on
your widget. A screen whose root is hit-testable across the whole screen counts as under the cursor
everywhere, and the widget is click-through everywhere over it, as it is without any flag. Where the
loader cannot tell (a cursor screen that is not on the menu layer, or the menu read switched off on a
game build it does not match) the widget is click-through too. If both flags are set,
`PauseMenuMouseInputs` wins. A loader older than v75 does not know the flag, so the widget stays
click-through there. Child windows inside the widget (`BeginChild`, a scrolling table) are ImGui
windows of their own and always take the mouse, covered or not.

To ask directly, `hooks->GameMenu->IsPauseMenuOpen()` reports the pause menu and
`hooks->UI->IsOverlayOpen()` reports a ModLoader window with exclusive capture.

---

## Config-Change Notifications

If you want to be notified whenever the user edits a value in your plugin's config via the
ModLoader UI, register a `PluginConfigChangedCallback`. Pass the `self` pointer your `PluginInit`
received -- the modloader uses it to scope the callback to your plugin's own config file, so you
are never notified about another plugin's config changes. `PluginShutdown` does not receive
`self`, so cache it in a static during `PluginInit`:

```cpp
static IPluginSelf* g_self = nullptr;

static void OnConfigChanged(const char* section, const char* key, const char* newValue)
{
    if (strcmp(section, "General") == 0 && strcmp(key, "Speed") == 0)
    {
        g_speed = strtof(newValue, nullptr);
    }
}

// In PluginInit:
g_self = self;
if (hooks->UI)
    hooks->UI->RegisterOnConfigChanged(self, OnConfigChanged);

// In PluginShutdown:
if (hooks->UI)
    hooks->UI->UnregisterOnConfigChanged(g_self, OnConfigChanged);
```

`newValue` is the raw string that was written to the INI file (e.g. `"true"`, `"42"`, `"3.14"`).

The callback runs on the render thread, before the INI is written, so take the value from
`newValue` rather than reading it back.

### Choices and saved state (v71)

Two config entry types change how the Config tab draws an entry:

```cpp
static const ConfigEntry CONFIG_ENTRIES[] = {
    // A dropdown. The description comes first, then each choice after a '|'.
    { "Map", "Style", ConfigValueType::Choice, "Dark", "Map colours|Dark|Light|Satellite" },

    // No field on the page. Your plugin writes it when the player drags the window.
    { "Map", "WindowX", ConfigValueType::Hidden, "-1", "Window position; -1 centres it" },
    { "Map", "WindowY", ConfigValueType::Hidden, "-1", "Window position; -1 centres it" },
};
```

- **Choice.** The INI stores the choice as written, and `newValue` is that text. The page matches
  the stored value ignoring case, and shows a value that is none of the choices as the default, so
  treat one the same way when you read it. The INI comment lists the choices for anyone editing
  the file by hand.
- **Hidden.** Values your plugin saves by itself, such as where a window was dragged. The page
  draws no field for them. Each section holding any ends with a **Saved state** row (its tooltip
  names them), and its **Clear** button sets each one back to its default, calling your
  config-change callback for every value it changes. The button is greyed out while they all hold
  their defaults. The value is plain text in the INI, so read and write it with whichever
  `Read*`/`Write*` suits it: a number counts as its default when it has the same value, so
  `WriteFloat`'s `-1.000000` matches a default of `"-1"`.

Listen for the Hidden keys if your plugin keeps their values in memory, so a Clear takes effect
straight away: put the window back, forget the remembered position.

Both are v71, and a v70 loader refuses a v71 plugin. A v71 loader built before these two types
were added draws a Choice as a text field, with the choices left in its description, and a Hidden
entry as a text field. The values still read and write as text, so nothing breaks; the page only
looks unfinished.

---

## Keybinds (Input sub-interface)

`hooks->Input` is also client-only (`nullptr` on server builds).

```cpp
static void OnF5Pressed(EModKey key, EModKeyEvent event)
{
    // toggle something
}

// In PluginInit:
if (hooks->Input)
    hooks->Input->RegisterKeybind(EModKey::F5, EModKeyEvent::Pressed, OnF5Pressed);

// In PluginShutdown:
if (hooks->Input)
    hooks->Input->UnregisterKeybind(EModKey::F5, EModKeyEvent::Pressed, OnF5Pressed);
```

Or register by UE key name string (case-insensitive):

```cpp
hooks->Input->RegisterKeybindByName("Insert", EModKeyEvent::Pressed, OnF5Pressed);
```

See the `EModKey` enum in `plugin_interface.h` for the full list of bindable keys. When the
ModLoader window is open, keyboard and raw mouse input is swallowed so camera movement stops —
keybind callbacks will still fire.

### Mouse wheel (v70)

Keybinds have no wheel keys, and `IModLoaderImGui::GetMouseWheel` only reads non-zero while a
ModLoader window has the cursor. For the wheel during normal gameplay, register a wheel handler.
Return `true` to consume the notch so the game never sees it:

```cpp
static bool OnWheel(const PluginMouseWheelEvent* ev, void* /*userData*/)
{
    // Ctrl+wheel zooms our map; everything else goes to the game.
    if (!(ev->modifiers & PluginWheelMod_Ctrl) || ev->delta == 0.0f)
        return false;

    g_zoom += ev->delta * 0.1f;   // one notch = 1.0; rawDelta has the unscaled value
    return true;
}

// In PluginInit:
if (hooks->Input)
    hooks->Input->RegisterMouseWheel(OnWheel, nullptr);

// In PluginShutdown:
if (hooks->Input)
    hooks->Input->UnregisterMouseWheel(OnWheel, nullptr);
```

- Each event carries the signed raw delta and the same value in notches, for both axes. Only
  one axis is non-zero per event, and touchpads send fractions of a notch. It also carries
  sided modifier bits (`PluginWheelMod_LeftCtrl`, `..._RightAlt`, and so on, plus the un-sided
  `PluginWheelMod_Ctrl` / `_Shift` / `_Alt` / `_Win` masks) and the cursor's screen position.
- Handlers run on the game thread, in registration order. The first one to return `true`
  stops the chain.
- While a ModLoader window has exclusive input capture, `ev->uiCapturing` is `true` and the
  return value is ignored, so ImGui keeps scrolling. The game is not receiving input then anyway.
- Registrations are dropped automatically when the plugin is unloaded or reloaded, so a stale
  handler cannot be called. Unregister in `PluginShutdown` anyway.

### Keys inside your own window (v71)

Keybinds are not dispatched while a text field has focus, so they cannot move a highlight in a
search list. `IModLoaderImGui::IsKeyPressed(vk, repeat)` reads ImGui's key state for a Win32
virtual-key code and works while you type. Call it from your render callback, only while your
window is focused, or it also fires for keys meant for the game:

```cpp
if (imgui->IsWindowFocused(0))
{
    if (imgui->IsKeyPressed(VK_DOWN, true)) ++g_highlight;
    if (imgui->IsKeyPressed(VK_UP, true))   --g_highlight;
}
```

Use the sided modifier codes (`VK_LSHIFT`, `VK_RCONTROL`, ...). `VK_SHIFT`, `VK_CONTROL`,
`VK_MENU` and the `VK_OEM_*` keys other than `VK_OEM_COMMA` and `VK_OEM_PERIOD` always report
false, and `VK_RETURN` is the main Enter key only.

### Sizing a popup (v74)

A modal opened with `BeginPopupModal` and no size of its own shrinks to fit its first frame, which
can be a strip a few pixels wide, and ImGui then saves that size. Two calls size the window that
opens next; call them right before `BeginPopupModal` (or a panel's own `Begin`), from your render
callback. There is no call to place it: ImGui centres a modal on the screen itself.

| Call | Description |
|------|-------------|
| `SetNextWindowSize(w, h, cond)` | Set the next window's size |
| `SetNextWindowSizeConstraints(min_w, min_h, max_w, max_h)` | Clamp it between a minimum and a maximum (no callback); `FLT_MAX` for no limit. A minimum width keeps an auto-sized window from collapsing |

`cond` is one of `PluginCond_Always`, `PluginCond_Once`, `PluginCond_FirstUseEver` or
`PluginCond_Appearing`. The recommended popup is as wide as its text needs but never narrower than a
minimum, and its size is not saved to the ini (`PluginWindowFlags_NoSavedSettings`), so a bad size
cannot stick; ImGui centres it:

```cpp
// Open it once, from a button: imgui->OpenPopup("Confirm##myplugin");
imgui->SetNextWindowSizeConstraints(360.0f, 0.0f, FLT_MAX, FLT_MAX);   // minimum width; needs <cfloat>
if (imgui->BeginPopupModal("Confirm##myplugin", nullptr,
        PluginWindowFlags_AlwaysAutoResize | PluginWindowFlags_NoSavedSettings))
{
    imgui->TextWrapped("A long message that wraps inside the minimum width.");
    if (imgui->Button("OK")) imgui->CloseCurrentPopup();
    imgui->EndPopup();
}
```

To give a window a fixed size instead, leave out `PluginWindowFlags_AlwaysAutoResize` and call
`SetNextWindowSize(w, h, PluginCond_Always)`. To fix only the width and keep the height auto-fitted, pass
0 for the height: `SetNextWindowSize(540.0f, 0.0f, PluginCond_Always)` overrides
`PluginWindowFlags_AlwaysAutoResize` on the axis it sets only. These calls need interface v74. A loader older than
that refuses a plugin that declares v74 and shows it as needing a newer mod loader, so a plugin
that has to run on a v73 loader declares 73 and leaves them out (see
`PLUGIN_INTERFACE_VERSION` in `plugin_interface.h`).

---

## IModLoaderImGui — Full API Reference

All functions take pre-formatted strings. Use `snprintf` to format before calling.

### Text

| Call | Description |
|------|-------------|
| `Text(text)` | Plain white text |
| `TextColored(r, g, b, a, text)` | Coloured text (0.0–1.0 floats) |
| `TextDisabled(text)` | Greyed-out text |
| `TextWrapped(text)` | Text that word-wraps at the window edge |
| `LabelText(label, text)` | Right-aligned label + value pair |
| `SeparatorText(label)` | Horizontal rule with an inline label |

### Inputs (return `true` when value changed)

| Call | Description |
|------|-------------|
| `InputText(label, buf, buf_size)` | Single-line text box |
| `InputInt(label, v, step, step_fast)` | Integer input with +/- buttons |
| `InputFloat(label, v, step, step_fast, format)` | Float input; pass `"%.3f"` etc. |
| `Checkbox(label, v)` | Boolean toggle |
| `SliderFloat(label, v, v_min, v_max, format)` | Float slider |
| `SliderInt(label, v, v_min, v_max, format)` | Integer slider |

### Buttons (return `true` when clicked)

| Call | Description |
|------|-------------|
| `Button(label)` | Standard-height button |
| `SmallButton(label)` | Inline small button |

### Layout

| Call | Description |
|------|-------------|
| `SameLine(offset, spacing)` | Next widget on same line; pass `0, -1` for defaults |
| `NewLine()` | Move to next line without any widget |
| `Separator()` | Horizontal rule |
| `Spacing()` | Blank vertical gap |
| `Indent(w)` | Indent following widgets; pass `0` for default indent |
| `Unindent(w)` | Undo indent |

### ID Stack

ImGui uses the label string as an ID. If you have multiple identical labels (e.g. two `"Delete"`
buttons), push a unique ID to disambiguate them:

```cpp
imgui->PushIDStr("item_0");
imgui->Button("Delete");
imgui->PopID();

imgui->PushIDStr("item_1");
imgui->Button("Delete");
imgui->PopID();
```

| Call | Description |
|------|-------------|
| `PushIDStr(str_id)` | Push a string onto the ID stack |
| `PushIDInt(int_id)` | Push an integer onto the ID stack |
| `PopID()` | Pop the most recent ID |

### Combo / Selectable

```cpp
static int s_selected = 0;
const char* options[] = { "Option A", "Option B", "Option C" };

if (imgui->BeginCombo("Mode", options[s_selected]))
{
    for (int i = 0; i < 3; ++i)
    {
        bool sel = (s_selected == i);
        if (imgui->Selectable(options[i], sel))
            s_selected = i;
    }
    imgui->EndCombo();
}
```

| Call | Description |
|------|-------------|
| `BeginCombo(label, preview)` | Open a combo box; returns true if open |
| `Selectable(label, selected)` | Item inside a combo (or standalone); returns true if clicked |
| `EndCombo()` | Must be called if `BeginCombo` returned true |

### Tree / Collapsible

```cpp
if (imgui->CollapsingHeader("Advanced"))
{
    imgui->Text("Hidden until expanded");
}

if (imgui->TreeNodeStr("Details"))
{
    imgui->Text("Sub-item content");
    imgui->TreePop();  // always call TreePop if TreeNodeStr returned true
}
```

| Call | Description |
|------|-------------|
| `CollapsingHeader(label)` | Section header that expands/collapses; no matching End call needed |
| `TreeNodeStr(label)` | Indented tree node; call `TreePop()` if it returns true |
| `TreePop()` | Close a tree node |

### Color Pickers

```cpp
static float col3[3] = { 1.0f, 0.5f, 0.0f };
imgui->ColorEdit3("Tint", col3);    // RGB

static float col4[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
imgui->ColorEdit4("Color", col4);   // RGBA
```

### Misc

| Call | Description |
|------|-------------|
| `SetTooltip(text)` | Show a tooltip when the previous widget is hovered |
| `IsItemHovered()` | True if the mouse is over the previous widget |
| `SetNextItemWidth(w)` | Override width of the next widget; pass `-1` to fill remaining space |
| `IsKeyPressed(vk, repeat)` | (v71) True on the frame a Win32 virtual key goes down; `repeat` adds the held-key repeats |
| `SetNextWindowSize(w, h, cond)` | (v74) Size the next window; see [Sizing a popup](#sizing-a-popup-v74) |
| `SetNextWindowSizeConstraints(min_w, min_h, max_w, max_h)` | (v74) Minimum and maximum size of the next window |

---

## Typical Panel Layout Pattern

```cpp
static void MyPanelRender(IModLoaderImGui* imgui)
{
    // Section header
    imgui->SeparatorText("Status");
    imgui->Text("Running: true");

    imgui->Spacing();

    // Settings section
    imgui->SeparatorText("Settings");

    static float radius = 500.0f;
    imgui->SetNextItemWidth(-1.0f);
    if (imgui->SliderFloat("Radius##myp", &radius, 0.0f, 5000.0f, "%.0f m"))
        SaveRadius(radius);

    static bool drawDebug = false;
    if (imgui->Checkbox("Draw debug", &drawDebug))
        g_drawDebug = drawDebug;

    imgui->Spacing();

    // Action buttons
    imgui->SeparatorText("Actions");
    if (imgui->Button("Reload data"))
        ReloadData();
    imgui->SameLine(0, -1);
    if (imgui->SmallButton("Clear cache"))
        ClearCache();
}
```

---

## Notes and Gotchas

- **No direct ImGui calls.** Never `#include "imgui/imgui.h"` or call `ImGui::` from a plugin.
  All calls must go through the `IModLoaderImGui*` table.

- **Callbacks are called on the render thread** (inside `IDXGISwapChain::Present`). Do not call
  SDK functions, allocate engine memory, or take locks that the game thread might be holding.
  Use `hooks->Engine->RegisterOnTick` for game-thread work and share state via an atomic, or a
  lock the game thread only holds briefly. Never hold that lock across slow game-thread work (a
  texture upload, a scene capture): the frame waits on it, and if that work waits on the frame the
  game hangs. If the lock can be busy for longer, `try_lock` it and skip that frame.

- **Static state is fine.** Panel and widget render callbacks run every frame (widgets always;
  panels while open). `static` local variables inside them persist across frames, which is the
  standard ImGui pattern for edit buffers and selection state.

- **Widget callbacks run every frame, even when the game is loading.** Guard against null or
  stale game-state pointers inside widget render callbacks; they are called more aggressively than
  panel callbacks.

- **Window titles must be unique.** ImGui uses the window title as an ID. If two plugins register
  panels with the same `windowTitle` they will share a window. Use your plugin name as a prefix
  (e.g. `"MyPlugin — Settings##myplugin"`).

- **IModLoaderImGui is valid for the lifetime of your callback.** Do not cache the pointer across
  frames or store it in globals — always use the pointer passed into the render callback.

- **`hooks->UI` and `hooks->Input` are nullptr on server builds.** Always null-check both
  pointers before calling any method on them.

- **`FreeTexture` is safe at any time (v70).** It retires the texture rather than destroying it:
  the handle stops drawing immediately, and the GPU resource and its slot are released once
  every frame that could have used them has finished. You can free from the game thread, from
  a render callback, or during a world transition, with no frame-counting or delays. The slot
  becomes reusable a frame or two later, so `GetFreeSlotCount` does not count it until then.
