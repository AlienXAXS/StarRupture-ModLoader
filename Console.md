# Console Plugin API (v63, all builds)

`IPluginConsole` (`hooks->Console`) lets a plugin add commands to the mod loader's own console
registry, and run command lines from code with the output delivered to a callback instead of a
console window. It is available from interface version 63 onwards and is non-null on every build.

---

## Table of Contents

- [Overview](#overview)
- [Registering a Command](#registering-a-command)
- [Writing Output](#writing-output)
- [The gameThread Flag](#the-gamethread-flag)
- [Capturing Output From Code](#capturing-output-from-code)
- [Lifetime Rules](#lifetime-rules)
- [Reference](#reference)
- [Full Example Plugin](#full-example-plugin)
- [Common Mistakes](#common-mistakes)

---

## Overview

The mod loader keeps one command registry with two front-ends:

- the ImGui developer console on client builds, and
- the Win32 console window on any build launched with `-console` -- the only console a dedicated
  server has.

A command registered once is available in whichever of those the user has, is listed by `help`
under your plugin's name, and completes with Tab in the `-console` window. These are mod loader
commands, not engine commands: the client console tries this registry first and falls through to
the engine console for anything it does not recognise, so engine commands are unaffected.

Command names are **case-insensitive and global** -- one namespace shared by the built-ins
(`help`, `plugins`, `reload`, `version`, ...) and every plugin. `RegisterCommand` returns `false`
rather than shadowing an existing name or alias, so check the return value and prefix anything
generic with your plugin's name.

---

## Registering a Command

```cpp
static void Cmd_Balance(const char* const* argv, int argc,
                        PluginConsoleSink sink, void* userData)
{
    IPluginConsole* console = static_cast<MyState*>(userData)->console;

    if (argc < 2)
    {
        console->Write(sink, PluginConsoleLineKind::Error, "usage: balance <player>");
        return;
    }

    console->Printf(sink, PluginConsoleLineKind::Output, "%s has %d credits",
                    argv[1], LookupBalance(argv[1]));
}

bool PluginInit(IPluginSelf* self)
{
    if (!self->hooks->Console)   // older loader
        return true;

    PluginConsoleCommandDesc desc = {};
    desc.name       = "balance";
    desc.aliases    = "bal money";
    desc.usage      = "balance <player>";
    desc.help       = "Show a player's credit balance";
    desc.handler    = Cmd_Balance;
    desc.userData   = &g_state;
    desc.gameThread = true;

    if (!self->hooks->Console->RegisterCommand(self, &desc))
        LOG_WARN("could not register 'balance' -- name already taken");

    return true;
}
```

`argv[0]` is the command name **exactly as typed**, which may be one of the aliases;
`argv[1..argc-1]` are the arguments. The line is tokenized on whitespace with `"quoted runs"`
collapsed into a single argument, so `balance "Some Player"` arrives as two entries. Both `argv`
and the strings it points at are valid for the duration of the call only -- copy anything you keep.

The loader copies every string in `desc`, so they do not have to outlive the call.

---

## Writing Output

The `PluginConsoleSink` your handler is given is an opaque handle to the console that is running
the command. Output is line-oriented and typed, so the front-end can colour it:

| Kind | Meaning | Rendering |
|------|---------|-----------|
| `PluginConsoleLineKind::Output` | normal command output | default |
| `PluginConsoleLineKind::Notice` | headers, hints, supporting text | dimmed |
| `PluginConsoleLineKind::Error` | something went wrong | red |

```cpp
console->Write (sink, PluginConsoleLineKind::Notice, "Players online:");
console->Printf(sink, PluginConsoleLineKind::Output, "  %-20s %4d ms", name, ping);
console->Clear (sink);   // wipe the scrollback, where the front-end has one
```

`Printf` formats into a 1024-byte buffer and truncates beyond that.

---

## The gameThread Flag

`desc.gameThread = true` makes the loader run your handler on the game thread, during the next
engine tick, instead of on the console thread that typed the command. **Set it for anything that
touches engine state** -- reading the world, the game state, actors, or calling a UFUNCTION. The
console threads are a raw input reader and the render thread; neither is safe for that, and
neither can block waiting for a tick.

The cost is that the command is *queued*: whatever typed it has already returned by the time your
handler runs, and your output appears a frame or two later. That is normal and both front-ends
handle it.

With `gameThread = false` the handler runs synchronously on whichever thread dispatched it, which
may be the render thread or the `-console` reader thread. Use it only for work that touches
nothing but your own data.

---

## Capturing Output From Code

`Execute` runs any command line -- yours, another plugin's, or a built-in -- and sends the output
to your callback rather than to a console window. This is the way to get a textual result out of a
command from code, for an RCON bridge, an HTTP route, or a chat command:

```cpp
struct Capture { std::string text; };

static void OnLine(PluginConsoleLineKind, const char* text, void* user)
{
    static_cast<Capture*>(user)->text += text;
    static_cast<Capture*>(user)->text += "\n";
}

static void OnDone(void* user)
{
    Capture* cap = static_cast<Capture*>(user);
    SendToRconClient(cap->text.c_str());
    delete cap;                      // owned until onComplete fires
}

Capture* cap = new Capture();
if (!self->hooks->Console->Execute(self, "plugins", OnLine, OnDone, cap))
{
    delete cap;                      // not a registered command; nothing ran
}
```

`Execute` returns `false`, having run nothing, when the first token is not a registered command --
that is the caller's cue to report an unknown command. It returns `true` once the command has
*started*: for a `gameThread` command both callbacks fire later, **on the game thread**, so they
must be thread-safe and `userData` must stay alive until `onComplete` has fired.

---

## Lifetime Rules

Three rules, each of which is a crash the loader had to make impossible:

**A sink is valid only inside the call it was handed to.** The front-end owns it. Stashing the
handle and writing through it from a worker thread a second later is a use-after-free; the loader
answers it by dropping the write and logging, but the output is still lost. Print everything
before returning.

**Unregister in `PluginShutdown`.** A handler address points into your module. The loader drops
every command a plugin owns before freeing its DLL, and re-validates a queued `gameThread` handler
before calling it, so a command that outlives its plugin is refused rather than fatal -- but that
is a safety net, not a design.

```cpp
void PluginShutdown()
{
    if (g_self && g_self->hooks && g_self->hooks->Console)
        g_self->hooks->Console->UnregisterAllCommands(g_self);
}
```

**Output can arrive on a thread you did not call from.** See
[The gameThread Flag](#the-gamethread-flag) above.

---

## Reference

### `PluginConsoleCommandDesc`

| Field | Type | Notes |
|-------|------|-------|
| `name` | `const char*` | Primary name. Case-insensitive, no spaces. Required. |
| `aliases` | `const char*` | Space-separated alternates, or null. |
| `usage` | `const char*` | e.g. `"balance <player>"`. Shown by `help`. May be null. |
| `help` | `const char*` | One line, shown by `help`. May be null. |
| `handler` | `PluginConsoleHandler` | Required. |
| `userData` | `void*` | Passed back to the handler. May be null. |
| `gameThread` | `bool` | Run on the next engine tick instead of the console thread. |

### `IPluginConsole`

| Function | Returns |
|----------|---------|
| `RegisterCommand(self, desc)` | `false` if a field is missing, or the name or an alias is taken |
| `UnregisterCommand(self, name)` | `false` if you do not own a command by that primary name |
| `UnregisterAllCommands(self)` | how many commands were removed |
| `HasCommand(name)` | `true` if any command, built-in or plugin, answers to that name or alias |
| `Write(sink, kind, text)` | -- |
| `Printf(sink, kind, format, ...)` | -- |
| `Clear(sink)` | -- |
| `Execute(self, line, onLine, onComplete, userData)` | `false` if the first token is not a command |

---

## Full Example Plugin

```cpp
#include "plugin_interface.h"
#include <string>

static IPluginSelf* g_self = nullptr;

static void Cmd_Hello(const char* const* argv, int argc,
                      PluginConsoleSink sink, void* /*userData*/)
{
    IPluginConsole* console = g_self->hooks->Console;

    if (argc >= 2)
        console->Printf(sink, PluginConsoleLineKind::Output, "Hello, %s!", argv[1]);
    else
        console->Write(sink, PluginConsoleLineKind::Notice, "Hello! (try: hello <name>)");
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self = self;

    if (!self->hooks->Console)
        return true;   // loader older than v63

    PluginConsoleCommandDesc desc = {};
    desc.name       = "hello";
    desc.usage      = "hello [name]";
    desc.help       = "Say hello";
    desc.handler    = Cmd_Hello;
    desc.gameThread = false;   // touches no engine state

    self->hooks->Console->RegisterCommand(self, &desc);
    return true;
}

extern "C" __declspec(dllexport) void PluginShutdown()
{
    if (g_self && g_self->hooks && g_self->hooks->Console)
        g_self->hooks->Console->UnregisterAllCommands(g_self);
}
```

---

## Common Mistakes

**Ignoring the return value of `RegisterCommand`.** Names are global. `status` or `list` is
almost certainly taken; your command then silently does not exist.

**Keeping the sink.** It belongs to the front-end and is only being served for the duration of
your handler. Writes through a stale sink are dropped with a warning in `modloader.log`.

**Touching engine state without `gameThread`.** A non-`gameThread` handler runs on the render
thread (client) or the console reader thread (`-console`). Reading the world from either is a
race, and usually a crash.

**Freeing `Execute`'s `userData` when `Execute` returns.** For a `gameThread` command it has not
run yet. Free it in `onComplete` -- and on the `false` return, where nothing ran and `onComplete`
will never fire.

**Assuming a console is open.** Registration always works, on every build; whether anyone can
type the command depends on the user having the ImGui console or having launched with `-console`.
`Execute` works either way.
