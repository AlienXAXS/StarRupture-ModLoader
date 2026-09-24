# Tests

Manual test projects. **Nothing in here is built by "Build Solution" or by CI.**

Each project appears in `StarRupture-ModLoader.sln` with `ActiveCfg` entries but no `Build.0`
entries -- which is what unchecking *Build* in Configuration Manager does. Visual Studio still shows
it, still gives it IntelliSense, and still builds it when you right-click the project and pick
**Build**. `msbuild StarRupture-ModLoader.sln` skips it, which is how `release.yml` stays unaffected.

If you ever need it from the command line, name the project file directly:

```
msbuild Tests\PreloadTest\PreloadTest.vcxproj /p:Configuration="Client Release" /p:Platform=x64
```

Output goes to `build\tests\`, flat rather than per-configuration.

> Building `Client` after `Server` (or the reverse) therefore **overwrites** `preload_test.dll`.
> That is safe -- the loader refuses a DLL built for the other executable and says so in the log --
> but if you are testing both, copy the one you want out before rebuilding.

---

## PreloadTest

A preload plugin that hooks `FEngineLoop::Init` and logs. The hook itself is deliberately the least
interesting thing it could do; the point is that getting there exercises the two pieces of the
preload path that are easy to break and hard to notice breaking.

### What it actually tests

`FEngineLoop::Init` is the one target that covers everything at once:

1. **The patch overlay** (`memory_scanner/patch_overlay.*`). The loader already hooks
   `FEngineLoop::Init`, and it installs that hook in Stage 1 *before* the preload phase runs. So by
   the time this plugin scans, the first 14+ bytes of the function have been replaced with a JMP
   stub -- and the AOB is anchored on exactly those bytes.

   If the overlay works, the pattern resolves anyway, because the scanner reads through the
   loader's own hooks and sees the image as it shipped. If it does not, the plugin is refused with
   *"pattern not found"*.

2. **The hook broker** (`hooks/hook_broker.*`). Installing here puts two detours on one address. If
   the broker works, both run: the loader's engine-init hook first (installed first), then this
   one, then the real function.

3. **The phase itself** -- discovery, version and target checks, the scan session, `PreloadInit`,
   and the log plumbing.

It is called exactly once, early, so a working test writes a handful of lines rather than flooding
the log.

### Why this AOB

The pattern is lifted verbatim from `ScanPatterns::FEngineLoop_Init` in
`hooks/game/scan_patterns.h` rather than dumped fresh, and that is the point: it is already
verified against the current game build by the loader's own preflight. So if this resolve fails
while the loader itself booted fine, the fault is in the preload path or the overlay -- not in the
pattern. A freshly dumped AOB would make a failure ambiguous.

`FEngineLoop::Init` sits outside the client/server conditionals in `scan_patterns.h`, so both build
targets use the same bytes.

### Running it

1. Right-click **PreloadTest** in Solution Explorer -> **Build**, in a `Client` or `Server`
   configuration matching the loader build you are testing.
2. Copy `build\tests\preload_test.dll` into `<game>\Binaries\Win64\ModLoader\Preload\`.
3. Launch.

### What success looks like

In `ModLoader\Logs\modloader.log`:

```
[Preload] Loading preload_test.dll
[Preload] PreloadTest v1.0.0 by modloader (priority 100)
[HookScan] PreloadTest: 1 pattern(s) resolved, no failures
[Plugin:PreloadTest] preload test starting -- game <version>, loader <tag>, module base 0x...
[Plugin:PreloadTest] FEngineLoop::Init resolved to exe+0x...
[HookBroker] 0x... now has 2 hooks -- 'FEngineLoop::Init' (PreloadTest) joined the chain behind modloader
[Plugin:PreloadTest] hook installed; expect a line from the detour when the engine starts
[Preload] PreloadTest is running (1 hook(s) installed)
...
[Plugin:PreloadTest] FEngineLoop::Init reached -- preload detour fired, chained behind the loader's own hook
[Plugin:PreloadTest] FEngineLoop::Init returned 0
```

The `[HookBroker] ... now has 2 hooks` line is the one that proves chaining; the last two prove the
detour actually fires. `hooks` in the console shows the same thing live:

```
> hooks
  exe+0x...  (2 hooks)
      1. modloader                engine_init
      2. PreloadTest              FEngineLoop::Init
```

### What failure looks like

| Symptom | Means |
|---|---|
| `pattern not found` in `hookfailures` | The overlay is not working -- the scan is seeing the loader's JMP stub. This is the regression the test exists to catch. |
| `pattern is not unique` | The AOB drifted after a game update. Re-sync it from `scan_patterns.h`. |
| No `[Preload]` lines at all | The phase did not run. Check for `-NoPreload`, `[Preload] Enabled=0`, or a leftover `ModLoader\Preload\.state`. |
| `wrong build target` | Client DLL on a server, or the reverse -- see the overwrite note above. |
| Plugin runs but the detour never logs | The chain is broken, or a link in front declined to call on. |

None of these stop the game booting: a preload plugin that fails is unloaded and the game starts
without it.
