# ImGui Integration — Plugin Developer Guide

The modloader owns Dear ImGui entirely. Plugins do **not** link against ImGui directly — all
rendering is done through a function-pointer table (`IModLoaderImGui`) passed into your render
callback. This keeps the ImGui version and DX12 backend entirely under the modloader's control.

The UI system is **client-only**. On server or generic builds `hooks->UI` and `hooks->Input` are
both `nullptr`. Always null-check before use.

---

## Availability

```ini
; modloader.ini — users can disable the overlay entirely
[UI]
Enabled=1       ; set to 0 to skip ImGui initialisation (no hooks, no rendering)
OpenKey=F2      ; key that opens/closes the ModLoader window
```

If `Enabled=0` your panel callbacks will never fire and `hooks->UI` will still be non-null, but
`RegisterPanel` is a no-op because the backend never starts.

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

bool PluginInit(IPluginLogger* logger, IPluginConfig* config,
                IPluginScanner* scanner, IPluginHooks* hooks)
{
    g_hooks = hooks;

    if (hooks->UI)
        g_panel = hooks->UI->RegisterPanel(&k_myPanel);
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

bool PluginInit(IPluginLogger* logger, IPluginConfig* config,
                IPluginScanner* scanner, IPluginHooks* hooks)
{
    g_hooks = hooks;

    if (hooks->UI)
        g_panel = hooks->UI->RegisterPanel(&k_myPanel);

    if (hooks->Input)
        hooks->Input->RegisterKeybind(EModKey::Insert, EModKeyEvent::Pressed, OnToggleKey);

    return true;
}
```

`SetPanelOpen` / `SetPanelClose` are silently ignored if the handle is null (e.g. UI backend
disabled). The panel does **not** need to be visible in the ModLoader Config tab first —
`SetPanelOpen` is equivalent to the user clicking the button there.

---

## Config-Change Notifications

If you want to be notified whenever the user edits a value in your plugin's config via the
ModLoader UI, register a `PluginConfigChangedCallback`:

```cpp
static void OnConfigChanged(const char* section, const char* key, const char* newValue)
{
    if (strcmp(section, "General") == 0 && strcmp(key, "Speed") == 0)
    {
        g_speed = strtof(newValue, nullptr);
    }
}

// In PluginInit:
if (hooks->UI)
    hooks->UI->RegisterOnConfigChanged(OnConfigChanged);

// In PluginShutdown:
if (hooks->UI)
    hooks->UI->UnregisterOnConfigChanged(OnConfigChanged);
```

`newValue` is the raw string that was written to the INI file (e.g. `"true"`, `"42"`, `"3.14"`).

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
  Use `hooks->Engine->RegisterOnTick` for game-thread work and share state via a lock or atomic.

- **Static state is fine.** Panel render callbacks run every frame while the panel is open.
  `static` local variables inside them persist across frames, which is the standard ImGui pattern
  for edit buffers and selection state.

- **Window titles must be unique.** ImGui uses the window title as an ID. If two plugins register
  panels with the same `windowTitle` they will share a window. Use your plugin name as a prefix
  (e.g. `"MyPlugin — Settings##myplugin"`).

- **IModLoaderImGui is valid for the lifetime of your callback.** Do not cache the pointer across
  frames or store it in globals — always use the pointer passed into the render callback.

- **`hooks->UI` and `hooks->Input` are nullptr on server builds.** Always null-check both
  pointers before calling any method on them.
