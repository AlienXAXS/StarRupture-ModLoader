# Preload Plugins

A **preload plugin** is a DLL in `ModLoader\Preload\` that runs during Stage 1 of loader startup:
after the game version check and the loader's own pattern preflight, and **before the game
executable's entry point has run at all**.

That is the only difference from an ordinary plugin, and it is the whole point. An ordinary
plugin's `PluginInit` happens once the engine is up, which is far too late to patch anything the
engine touches on the way there.

SDK and starter project: **[StarRupture-PreLoadPlugin-SDK](https://github.com/AlienXAXS/StarRupture-PreLoadPlugin-SDK)**.

---

## What exists at preload time, and what does not

The game's main thread is parked inside `MainInitApcProc`, which fires from `NtTestAlert` at the
end of process initialisation. At that moment:

| Available | Not available |
|---|---|
| The game `.exe`, fully mapped | `GMalloc` — the CRT static initialisers have not run |
| Pattern scanning over it | `FEngineLoop::PreInit`, `GEngine`, any world |
| Byte patching and detours | `UObject`, `FName`, `FString`, `GConfig` |
| `self->logger` | `self->hooks`, `self->config` (both null) |
| The loader's splash window | The ImGui overlay, the console, plugin networking |

So: **resolve, patch, and return.** Do not touch the engine, do not allocate through it, do not
start threads that assume it exists, and do not block — the whole game is waiting on you, with a
hard 180-second ceiling before the loader gives up and lets it boot regardless.

Your detours fire later, on the game thread, once the engine is running. That is normal and is what
they are for.

---

## Refusal is cheap, and the game never pays for it

A preload plugin that fails anything is unloaded, reported and skipped. The loader carries on and
the game starts normally. "Anything" means:

- an interface version outside `[PRELOAD_INTERFACE_VERSION_MIN, MAX]`
- the wrong build target (a client DLL on a dedicated server, or the reverse)
- a pattern that does not resolve
- a pattern that resolves **more than once**
- a pattern that resolves to the **wrong kind of thing**
- a crash in any of its entry points

This is deliberately the opposite of `PatternPreflight`, which disables the **whole mod loader**
when one of the loader's own patterns breaks. A preload plugin is somebody's mod, and an outdated
one has to be safe to leave installed across a game update: it stops working, it says so, and
nothing else changes.

---

## Two phases, and the split is not decorative

```cpp
void PreloadScan(IPluginSelf* self, IPluginHookScanner* scanner);  // resolve. nothing else.
bool PreloadInit(IPluginSelf* self, IPreloadPatch* patch);         // install.
```

`PreloadInit` is only ever called when **every** pattern in `PreloadScan` resolved cleanly, and
`patch` is not available during the scan.

That is what makes refusal safe. If a plugin installed a detour while resolving and a later pattern
then missed, the loader would have to free a module that has already written a jump into game code
— and it cannot undo that, or even find out it happened. So the interface does not hand you the
means.

The scanner is the same `IPluginHookScanner` ordinary plugins get, with the same rules (see
`plugins/plugin_interface.h`), the same report, the same failure window and the same `hookfailures`
console command.

---

## Pattern rules (shared with ordinary plugins, v68)

Both of these refuse the plugin. There is no flag to turn either off.

**1. A pattern must match exactly once.** Two matches is not an address, it is a coin flip. Before
v68 the first match won silently and the offset was then written into `scan_cache.ini`, so a
pattern that stopped being unique after a game update resolved to whichever copy sat lower in the
image, on every launch, with nothing looking wrong.

**2. The address must be what you said it is.** Declare a `PluginScanKind` and the loader checks it
against the executable's structure before handing it back:

| Kind | Checked against |
|---|---|
| `PLUGIN_SCAN_FUNCTION_START` | The exception directory (`.pdata`): must be a function's primary entry, not a separated cold chunk, and at least 14 bytes long so a detour fits |
| `PLUGIN_SCAN_IN_FUNCTION` | Inside some function that has unwind info — for mid-function anchors |
| `PLUGIN_SCAN_CODE` | An executable section — for hand-written thunks with no unwind info |
| `PLUGIN_SCAN_DATA` | An initialised, non-executable section |
| `PLUGIN_SCAN_VTABLE` | Data, and the first `vtableSlots` pointers each point at a function start |
| `PLUGIN_SCAN_ANY` | Nothing. Uniqueness only. The report labels it unvalidated |

`PLUGIN_SCAN_UNSPECIFIED` is 0 and is **refused** — a request that forgot to say what it wants does
not quietly get the weakest check.

When something fails, the report names every match and what it landed on:

```
UCrCrafter::FinishCrafting  [required]
    pattern is not unique -- it matched 3 times and an AOB must resolve to exactly one address.
    Pattern: 48 89 5C 24 ?? 57 48 83 EC ??
      #1  StarRupture-Win64-Shipping.exe+0x3F219B0  .text  function start (0x1A4 bytes)
      #2  StarRupture-Win64-Shipping.exe+0x41C2A37  .text  inside function +0x41C2A00 (+0x37)
      #3  StarRupture-Win64-Shipping.exe+0x52B1104  .rdata
```

Those RVAs paste straight into IDA. If a symbol is resolvable (a `.pdb` next to the game exe), the
name is appended too.

`resultOffset` is the fix for "landed 0x37 bytes inside a function": set it to `-0x37`.

---

## Minimal plugin

```cpp
#include "preload_interface.h"

static PreloadInfo s_info = {
    PRELOAD_INTERFACE_VERSION,
    "MyPreload", "1.0.0", "me",
    "Patches a thing before the engine starts",
    PRELOAD_TARGET_CLIENT,
    100                       // priority: lower runs first
};

static uintptr_t s_target   = 0;
static void*     s_original = nullptr;

extern "C" __declspec(dllexport) PreloadInfo* GetPreloadInfo() { return &s_info; }

extern "C" __declspec(dllexport)
void PreloadScan(IPluginSelf* self, IPluginHookScanner* scanner)
{
    PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName = "FEngineLoop::PreInit";
    req.pattern  = "48 89 5C 24 ?? 57 48 83 EC ??";
    req.kind     = PLUGIN_SCAN_FUNCTION_START;

    s_target = scanner->Resolve(self, &req);   // 0 on failure; already reported
}

extern "C" __declspec(dllexport)
bool PreloadInit(IPluginSelf* self, IPreloadPatch* patch)
{
    self->logger->Info(self, "game build %s", patch->GetGameVersion());
    return patch->InstallHook(self, "FEngineLoop::PreInit", s_target, &MyDetour, &s_original);
}

extern "C" __declspec(dllexport) void PreloadShutdown() {}   // optional
```

`GetPreloadInfo` and `PreloadInit` are required. `PreloadScan` and `PreloadShutdown` are optional —
a plugin that resolves no patterns simply does not export one.

---

## `IPreloadPatch`

Deliberately small; there is no engine to expose.

```cpp
HMODULE     GetGameModule();
uintptr_t   GetGameModuleBase();
size_t      GetGameModuleSize();
const char* GetGameVersion();        // the game's ProductVersion
const char* GetLoaderBuildTag();     // "dev" on local builds

bool ReadBytes (uintptr_t address, void* dest, size_t size);
bool WriteBytes(uintptr_t address, const void* source, size_t size);
bool Nop       (uintptr_t address, size_t size);

bool InstallHook(const IPluginSelf* self, const char* name, uintptr_t target,
                 void* detour, void** outOriginal);
bool RemoveHook (const IPluginSelf* self, const char* name);
```

All three memory functions handle page protection themselves and refuse an address outside a loaded
module, so a bad offset is a `false` return rather than an access violation before the game has
even started. `InstallHook` refuses a target of `0` for the same reason.

**Hooks are owned by the loader, not by your plugin.** They are registered under the owning plugin
and taken back out at shutdown, and if `PreloadInit` crashes part-way through, whatever it already
installed is removed before the module is freed.

---

## Ordering

Plugins run in `priority` order (lower first), ties broken by file name. It matters when two
preload plugins patch the same function: whoever installs second detours whatever the first one
left behind. `100` is a reasonable "no opinion" value.

This needs two passes over the folder — a DLL has to be loaded before it can be asked what its
priority is — so the loader loads and vets everything first, sorts, then scans and installs.

Preload plugins are installed **after** the loader's own hooks. The loader's subsystems then never
depend on anything a third-party DLL did, and a preload detour over a loader detour chains
correctly through the trampoline.

---

## When it goes wrong

A preload plugin is the one thing in the loader that can hang a game before anything exists to
report it. Three things exist for that.

### The breadcrumb

`ModLoader\Preload\.state` holds the file name of whichever DLL is currently being touched, flushed
to disk, and deleted the moment that DLL is through. If the file is still there on the next launch,
that plugin did not survive its turn — so it is **skipped**, and the log says so:

```
[Preload] MyPreload.dll: SKIPPED -- it did not finish loading on the previous launch.
[Preload]   The game most likely hung or crashed inside it before the engine started.
[Preload]   Delete ModLoader\Preload\.state to let it try again.
```

That turns "the game won't start and the log stops mid-sentence" into a second launch that works
and names the culprit.

### Kill switches

```
-NoPreload                              command line; skips the phase entirely
```

```ini
[Preload]
Enabled=0                               ; skip the phase
Disabled=BadPlugin.dll,Other.dll        ; skip named plugins
```

Read straight from `ModLoader\modloader.ini` with the raw profile API — the config manager does not
exist yet at Stage 1.

### Seeing what happened

The phase runs before anything that could display its result. `PreloadManager` therefore keeps its
records for the whole session:

```
> preload
3 preload plugin(s), 2 running:
  EarlyPatch               1.2.0      running                      EarlyPatch.dll
      2 hook(s) installed
  ConfigTweak              0.9.1      running                      ConfigTweak.dll
  OldMod                   1.0.0      pattern scan failed          OldMod.dll
      one or more patterns did not resolve -- run `hookfailures` for the detail
```

On a dedicated server (`-console`) this is the only way to see it without reading the log. On the
client, failures also appear in the plugin hook failure window at the main menu, tagged
`[preload]`.

---

## Loader-side layout

| File | What it does |
|---|---|
| `preload/preload_interface.h` | The ABI. Also shipped in the SDK repo |
| `preload/preload_manager.*` | Discovery, vetting, the two passes, isolation, breadcrumb |
| `preload/preload_patch.*` | `IPreloadPatch` and the per-plugin hook registry |
| `memory_scanner/scan_validation.*` | Uniqueness + kind checking, shared with ordinary plugins |
| `memory_scanner/image_info.*` | Section table and `.pdata` lookups behind the kind checks |

The phase is `PreloadPhase()` in `core/init_phases.cpp`, called from `MainInitThreadProc` right
after `InstallHooksPhase()` and before `ReleaseMainThread()`.

### It only runs on the parked path

`RunPhase()` does nothing unless `g_mainThreadParked` is set. On the injected path the main thread
was stopped with `SuspendThread` at an arbitrary instruction — routinely inside `LdrLoadDll` or
`RtlAllocateHeap` — and `LoadLibrary` from the init thread can deadlock against the loader lock it
is holding. That is the v1.16.0 hang, and it is not worth re-creating for a phase whose entire
value is already gone by then: on that path the game has been running for a while.
