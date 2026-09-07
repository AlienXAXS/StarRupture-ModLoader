# Game Menu Plugin API (v64, client only)

`IPluginGameMenu` (`hooks->GameMenu`) puts a row in the game's own main menu and pause menu --
next to NEW GAME, OPTIONS and CREDITS, not in a mod loader window. It is available from interface
version 64 onwards and is **`nullptr` on server and generic builds**, so null-check it.

---

## Table of Contents

- [Overview](#overview)
- [Adding a Row](#adding-a-row)
- [Where the Row Goes](#where-the-row-goes)
- [Lifetime Rules](#lifetime-rules)
- [When It Is Not Available](#when-it-is-not-available)
- [Reference](#reference)
- [Full Example Plugin](#full-example-plugin)
- [Common Mistakes](#common-mistakes)

---

## Overview

The mod loader adds one row of its own -- **MOD LOADER**, between OPTIONS and CREDITS, opening the
overlay -- and `AddEntry` puts yours alongside it.

Rows are not drawn by the mod loader. The loader splices an entry into the list the menu widget
already builds its buttons from, so the game creates the button itself: same widget class, same
style, same hover and click sounds, and it takes part in gamepad and keyboard navigation like any
other row. There is no ImGui involved and nothing to render each frame.

A row added while a menu is on screen appears the **next time that menu is built**, not
immediately. Register in `PluginInit` and it is there from the first main menu.

The user can turn the mod loader's own row off with `[UI] GameMenuEntry=0` in `modloader.ini`.
That setting does not affect plugin rows.

---

## Adding a Row

```cpp
static GameMenuEntryHandle g_menuRow = nullptr;

static void OnMyRowClicked(void* userData)
{
    // Runs on the GAME THREAD, inside the menu's click handler. Be quick.
    auto* self = static_cast<IPluginSelf*>(userData);
    OpenMyPanel();
}

bool PluginInit(IPluginSelf* self)
{
    if (self->hooks->GameMenu && self->hooks->GameMenu->IsAvailable())
    {
        PluginGameMenuEntryDesc desc{};
        desc.id       = "teleporter";                 // unique within your plugin
        desc.label    = "TELEPORTER";                 // button text
        desc.targets  = PLUGIN_GAME_MENU_PAUSE;       // pause menu only
        desc.anchor   = PLUGIN_GAME_MENU_ANCHOR_BOTTOM;
        desc.onClick  = OnMyRowClicked;
        desc.userData = self;

        g_menuRow = self->hooks->GameMenu->AddEntry(self, &desc);
    }
    return true;
}

void PluginShutdown()
{
    if (g_hooks && g_hooks->GameMenu)
        g_hooks->GameMenu->RemoveAllEntries(g_self);
}
```

Every string in the descriptor is **copied**, so they need not outlive the call.

`AddEntry` returns `nullptr` when the descriptor is incomplete, when you have already used that
`id`, or when all entry slots are taken. Labels are ASCII; the game's button style upper-cases
them, so write them upper-cased anyway.

---

## Where the Row Goes

`anchor` is a named position, not an index:

| Anchor | Position |
| --- | --- |
| `PLUGIN_GAME_MENU_ANCHOR_TOP` | above the first stock row (above NEW GAME) |
| `PLUGIN_GAME_MENU_ANCHOR_AFTER_OPTIONS` | directly below OPTIONS |
| `PLUGIN_GAME_MENU_ANCHOR_BOTTOM` | below the last stock row (above QUIT) |

Named rather than numeric because the stock rows change between game versions, and an index that
means "after Options" in one build means something else in the next. If a menu turns out to have
no OPTIONS row at all, an `AFTER_OPTIONS` entry falls back to the bottom -- a row in the wrong
place beats a row that silently never appears.

Several entries sharing an anchor appear in registration order.

One thing to know about `TOP`: when a menu is opened with a gamepad or the keyboard it focuses
its first row, so a `TOP` entry becomes the default selection. Use it only when that is what you
want.

`targets` is a bitmask:

| Target | Menu |
| --- | --- |
| `PLUGIN_GAME_MENU_MAIN` | the title screen menu |
| `PLUGIN_GAME_MENU_PAUSE` | the in-game pause menu |

CONTINUE and QUIT are separate widgets rather than list rows, so nothing can be placed above
CONTINUE or below QUIT.

---

## Lifetime Rules

**`onClick` runs on the game thread.** It is called from inside the menu widget's click handler,
on the tick the button was pressed. Opening a window or setting a flag is the shape this is for;
anything slow blocks the frame.

**Remove your rows in `PluginShutdown`.** `onClick` is an address inside your module. The loader
drops a plugin's rows before `FreeLibrary` and re-checks that the address is still in a loaded
module before calling it -- but neither is a reason to leave one registered.

**`userData` must outlive the row.** It is stored as-is and handed straight back.

**A reload starts clean.** The loader forgets your rows when your DLL is freed, so re-register in
`PluginInit` rather than assuming the previous registration survived.

---

## When It Is Not Available

Two things can make `IsAvailable()` return `false`:

- **This is a server or generic build.** `hooks->GameMenu` is `nullptr` there; there are no menus
  to add to. Null-check before the `IsAvailable()` call.
- **The menu patterns did not resolve.** The injection rests on two AOB signatures, and a game
  update can move them. That is deliberately not fatal to the loader -- it just means no rows
  appear, and `modloader.log` says so.

Entries registered while it is unavailable are accepted and simply never shown. That is fine for a
convenience row, but if a menu row is the *only* way into your UI, check `IsAvailable()` and offer
a keybind as well.

---

## Reference

```c
typedef void* GameMenuEntryHandle;
typedef void (*PluginGameMenuCallback)(void* userData);

enum PluginGameMenuTarget : int {
    PLUGIN_GAME_MENU_MAIN  = 1 << 0,
    PLUGIN_GAME_MENU_PAUSE = 1 << 1,
};

enum PluginGameMenuAnchor : int {
    PLUGIN_GAME_MENU_ANCHOR_TOP           = 0,
    PLUGIN_GAME_MENU_ANCHOR_AFTER_OPTIONS = 1,
    PLUGIN_GAME_MENU_ANCHOR_BOTTOM        = 2,
};

struct PluginGameMenuEntryDesc {
    const char*            id;         // unique within your plugin, ASCII, stable
    const char*            label;      // button text, ASCII
    int                    targets;    // bitmask of PluginGameMenuTarget
    int                    anchor;     // PluginGameMenuAnchor
    PluginGameMenuCallback onClick;    // called on the game thread
    void*                  userData;   // passed straight back to onClick
};

struct IPluginGameMenu {
    GameMenuEntryHandle (*AddEntry)(const IPluginSelf* self, const PluginGameMenuEntryDesc* desc);
    bool                (*RemoveEntry)(const IPluginSelf* self, GameMenuEntryHandle handle);
    int                 (*RemoveAllEntries)(const IPluginSelf* self);
    bool                (*IsAvailable)();
};
```

---

## Full Example Plugin

```cpp
#include "plugin_interface.h"

static IPluginSelf*        g_self    = nullptr;
static IPluginHooks*       g_hooks   = nullptr;
static GameMenuEntryHandle g_menuRow = nullptr;

static PluginInfo g_info = {
    "MenuDemo", "1.0.0", "you", "Adds a row to the game's menus",
    PLUGIN_INTERFACE_VERSION, PLUGIN_TARGET_CLIENT
};

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo() { return &g_info; }

static void OnRowClicked(void*)
{
    g_self->logger->Info("Menu row clicked");
    // Open a panel, toggle a widget, set a flag -- something short.
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self  = self;
    g_hooks = self->hooks;

    if (!g_hooks->GameMenu)
    {
        self->logger->Info("No game menu on this build");
        return true;
    }

    if (!g_hooks->GameMenu->IsAvailable())
        self->logger->Warn("Game menu injection unavailable -- use the keybind instead");

    PluginGameMenuEntryDesc desc{};
    desc.id      = "demo";
    desc.label   = "MENU DEMO";
    desc.targets = PLUGIN_GAME_MENU_MAIN | PLUGIN_GAME_MENU_PAUSE;
    desc.anchor  = PLUGIN_GAME_MENU_ANCHOR_BOTTOM;
    desc.onClick = OnRowClicked;

    g_menuRow = g_hooks->GameMenu->AddEntry(self, &desc);
    if (!g_menuRow)
        self->logger->Error("Failed to add the menu row");

    return true;
}

extern "C" __declspec(dllexport) void PluginShutdown()
{
    if (g_hooks && g_hooks->GameMenu)
        g_hooks->GameMenu->RemoveAllEntries(g_self);
    g_menuRow = nullptr;
}
```

---

## Common Mistakes

**Not null-checking `hooks->GameMenu`.** It is `nullptr` on server and generic builds. A plugin
declaring `PLUGIN_TARGET_CLIENT` will not load on a server, but shared code between a client and a
server plugin often forgets.

**Doing work in `onClick`.** It runs on the game thread inside the click handler. Set a flag or
open a window and return.

**Expecting the row to appear immediately.** Menus are built when they open. A row registered
while the pause menu is already up shows the next time it is opened.

**Reusing an `id`.** Ids are per-plugin and `AddEntry` refuses a duplicate rather than replacing
the existing row. Check the return value.

**Leaving rows registered in `PluginShutdown`.** The loader cleans up before `FreeLibrary` and the
click path re-validates the callback address, but a row that outlives its DLL is not a state worth
reaching.
