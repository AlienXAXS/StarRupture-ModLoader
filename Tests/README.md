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

---

## ConfigEditTests

A console program, not a plugin: it runs on its own, with no game and no DLLs to copy anywhere.

It compiles the loader's own `config/config_edit.cpp`, `config/config_manager.cpp` and
`UI/plugin_panel_registry.cpp` and replaces only what those call out to (the keybind registry, the
plugin list, the logger, the window theme -- see `test_doubles.cpp`). It covers:

- `SetLive` changes the value and notifies without touching the file; `Commit` writes it; a second
  `Commit` with nothing changed does not rewrite the file.
- A handler that answers a change by calling `SetLive` again (a clamp) does not deadlock.
- Committing a changed Keybind moves the registration and the Block state to the new combo.
- A plugin's own `IPluginConfig::Write*` shows up in `ConfigEdit::Get` and the Config tab's copy, and
  does not fire `OnConfigChanged`.
- A handler that clamps a value by calling `IPluginConfig::Write*` has its clamped value persisted by `Commit`.
- A hand-edited INI that spells a section or key in another case still applies its Block state on load.
- Forgetting a plugin's `OnConfigChanged` callbacks drops its own and leaves the next plugin's alone, and `FreeShutDownPlugin` (which every unload and reload frees through) calls `ForgetRegistrations` before it waits at the call gate and again before the free, which in turn calls that. The loader's plugin manager cannot be linked into a console program, so the second half is checked by reading `plugin_manager.cpp`; deleting any of those calls fails the test.

Build it from the command line (it creates `ModLoader\Plugins\config` beside the exe, under
`build\tests`, and removes its own INI files when it finishes):

```
msbuild Tests\ConfigEditTests\ConfigEditTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\config_edit_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## PanelUnloadTests

A console program like ConfigEditTests. It compiles the loader's own `UI/plugin_panel_registry.cpp`
and `UI/plugin_widget_registry.cpp` and replaces only what those call out to (see
`test_doubles.cpp`). It covers:

- `ForgetModule` drops one module's panels, widgets and panel-closed callbacks and leaves another
  module's alone; a dropped handle is harmless, and the title can be registered again.
- An unload that races the render thread waits out a panel render or panel-closed callback still
  inside the module; a render that never returns makes `ForgetModule` give up and say so; a
  `renderFn` may unregister its own panel; a panel forgotten mid-frame is not drawn.
- `FreeShutDownPlugin`, which every unload and reload frees through, calls `ForgetRegistrations` (both
  registries' `ForgetModule`) before it waits at the call gate, and keeps the module when the gate does
  not free it. As with ConfigEditTests, this is checked by reading `plugin_manager.cpp`.
- The call gate (`plugins/plugin_call_gate.cpp`), with the free step replaced: nothing is freed while a
  thread holds a callback it copied from a registry but has not called yet, or while a widget is being
  drawn; a running call can nest while a free waits; a new call and a second free are held back until the
  free is done; a free asked for from inside a call is refused at once (in under a second, not after the
  full wait); an exception leaves the gate open.
- Every dispatch site takes the call gate before it reads its registry, and an HTTP handler or filter is
  copied and then called with no registry lock held. Checked by reading the sources, 44 sites and the HTTP
  locks. No test runs an HTTP request through the hook.

```
msbuild Tests\PanelUnloadTests\PanelUnloadTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\panel_unload_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## LayoutCheckTests

A console program like ConfigEditTests. It compiles the loader's own `core/layout_check.cpp` and
`core/compat_report.cpp` without `MODLOADER_CLIENT_BUILD`, which keeps the comparison and the
report's JSON and drops the parts that touch the game, so nothing else is linked.

The layout tables themselves are checked by CI (`.github/workflows/sr-layout-drift.yml`): it makes
each `core/layout_table_<CL>.generated.h` again from the loader's sources and the Game-SDK commit it
was made from (the submodule for the newest, `tools/layout-table-pins.json` for an older one) and
diffs it with the committed file. The server's table (`core/layout_table_server_<CL>.generated.h`) is
checked the same way by a second job that runs `tools/Update-LayoutTable.ps1 -Server` itself, so the files
the server build compiles are read from the project file in one place. A table that is out of date fails
the run; fix it with `tools/Update-LayoutTable.ps1`.

`fixtures/` holds two layout tables generated from the Game-SDK at CL 126119 and CL 127004 for
`ACrCharacterPlayerBase` and its reflected bases (see `tools/Update-LayoutTable.ps1` for the
generator; these were made with `--used` naming just that class and `--namespace
SrLayoutTable126119` / `SrLayoutTable127004`). One table plays the SDK the loader was built against,
the other the running game. It covers:

- The 127004 SDK on a 127004 game: every class checked, 0 mismatched, nothing logged.
- The 126119 SDK on a 127004 game: `ACrCharacterPlayerBase` is the one changed class, with its size
  (0x15E0 to 0x15F0) and `ADSMoveSpeed` (0x13A8 to 0x13B8) among the lines; its bases are unchanged.
- The same mismatch forced through: `FoundChanges` is true (and false for the matching
  build), and the compatibility report's JSON carries the ProductVersion, the exe hash, a missed
  pattern with its quotes escaped, each global's RVA, and the class size and `ADSMoveSpeed` offset,
  old and new. The CL in the report's file name comes from the ProductVersion.
- Static arrays: the loader's own `core/layout_table_130045.generated.h` holds `UWorld::LineBatchers[4]` as
  0x20 bytes, the whole member, which matches a game reflecting `ElementSize` 8 times `ArrayDim` 4;
  the size of one element, as Dumper-7 comments it, is the old false positive.
- The loader's CL-130045 table on a 130045 game (nothing changed) and on a 127004 game, where
  `UCrEnviroWaveTimerSubsystem::TimerActor` and `ACrCrafter::CraftComponent` sit at the older offsets:
  both count as changes without a per-build rule and none with it, and the verdict is not a safe-off.
- The same CL-130045 table against the real 127004 layout of the classes and members the loader's table
  from v1.26.1 lists (kept in `fixtures/layout_table_loader_127004.h`; the tables list more since): the
  same two members differ and nothing else, so a 127004 game has no changed class. The v1.26.1 table
  against a 130045 game, the other way round, finds `TimerActor` as a changed class.
- One table per game build: which table a running CL picks (its own, else the newest, whether the CL sits
  between two tables, above the newest, below the oldest or is 0), that the list of tables is strictly
  increasing, that each table's class and row counts are what its header says, and that a 127004 run selects
  the 127004 table, which finds nothing changed on the real 127004 layout and needs no per-build rule. The
  two tables list the same classes except the ones only the 130045 SDK has, and no class a plugin keeps
  compiled in differs between them except in per-build members, and that comparison saw at least one such
  class. Members that moved between the builds in other classes (the loader's own, the menu layer, the
  version-map keys) are only counted, not asserted: each build is compared with its own table, and a move
  there only matters to a build that has no table of its own.
- The server's table (`core/layout_table_server_127004.generated.h`): one table, with the counts its header
  states; no class is tagged for the menu layer and the client's menu classes are not in it; it finds nothing
  changed on its own layout; a moved member of a class the loader's own code reads (`UWorld`) or a missing
  class is a safe-off, and a moved member of a class only a version map key names is listed as changed with no
  safe-off. The test build is a client build, so it reads the client's list of tables.
- A member the game does not reflect (`ULevel::Actors`, listed in `kUnreflectedMembers`): the table generator
  leaves it out, so none of the three tables has the row and each finds nothing changed on a game without it, with
  nothing skipped. The list stays as a net for a member the generator misses, so the same cases run on each table
  with the row put back: a game that has every member but that one finds nothing changed, with the skipped row
  listed; another member of the same class is still compared, the same member name on another class is still
  reported, a moved core member is still a safe-off, and a game that does reflect `ULevel::Actors` at
  another offset gets a mismatch.
- The `UObject::ProcessEvent` check: a stub virtual table whose slot holds the function the map resolved
  passes; one that holds another function adds one line to the result, is a found change with no class
  named, and gives a safe-off verdict; a different function in the next slot does not matter.
- One synthetic case for each kind of difference: a missing class, alignment, a missing member, a
  member size, a bit-field mask, a bool that stopped or started being one; a Blueprint class and a
  class of unknown kind are deferred, and a member size of 0 is not compared.
- A type reflection did not find: an `F` name nothing or only a class carries (`FTexture` beside
  `UTexture`) is a plain C++ type; an `F` name a struct carries, or a `U` or `A` name anything
  carries, is a request that missed.

```
msbuild Tests\LayoutCheckTests\LayoutCheckTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\layout_check_tests.exe
```

It prints one line per check and exits 0 when every check passes.

## NetCompatTests

A console program like LayoutCheckTests, for the rule that decides whether a plugin's packets flow
between a client and a server loader whose plugin versions differ. It compiles the loader's own
`network_channel/wire_compat.h`, which has no game or Windows dependency, so nothing is linked. It
covers:

- The rule: equal version strings; equal wire id on two declared sides; the server's accept list
  naming the client's version and the client's list naming the server's; neither side declared;
  one side declared; a different plugin name (never matched by name alone); list elements matched
  whole, never as a prefix or substring.
- Accept lists: empty, garbage, duplicate, oversized and separator-bearing lists.
- The wire table the two loaders swap: a round trip, every truncation point, an empty or garbage
  table, unknown records and extra fields, and the ack with and without a tail.
- The pair state the send, ready and receive paths share, and the once-per-connection warning keys.
- The arguments of the two sends that name their target plugin, including that no name they accept
  can be the loader's own.

```
msbuild Tests\NetCompatTests\NetCompatTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\net_compat_tests.exe
```

It prints one line per check and exits 0 when every check passes.

## PluginNeedTests

A compile test for `plugins/plugin_interface.h`, which hides every host-table member added after the
interface level a plugin declares with `PLUGIN_NEEDS_INTERFACE` (default: 75). It builds one small
source file per level (72 to 82), one file with nothing set, and one with
`PLUGIN_INTERFACE_HOST_BUILD` (what the loader itself uses). Each file includes `need_checks.h`,
which checks with `static_assert` that:

- a member added at version N is visible exactly when the level is N or more,
- a visible member sits at its frozen offset, and each host table's `sizeof` ends at its last
  visible member,
- the structs a plugin allocates (`PluginInfo`, `PluginPanelDesc`) are never cut down.

`need_default.cpp` also checks that a plugin which sets nothing declares 75 and cannot name
`Images`, `HostRules`, `Registry`, `DL_AddTexturedTriangles` or `GetServerAddress`.

`opt_73_80.cpp`, `opt_75_77.cpp` and `opt_default.cpp` do the same for the optional-member accessors (`PLUGIN_OPTIONAL_INTERFACE`, `PluginOptRegistry` and the others): an accessor exists exactly when the optional level reaches its member, and the offsets it reads match the table where the member is visible.

There is nothing to run: it is a static library, and the build is the test.

```
msbuild Tests\PluginNeedTests\PluginNeedTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
```

## PluginHostInfoTests

A console program for the loader's call to a plugin's optional `PluginSetHostInfo` export and what the plugin's side does with it. A stand-in plugin declares need 73 and optional 80, exports the function with `PLUGIN_EXPORT_HOST_INFO()`, and the loader's own `plugins/plugin_host_info.h` (the function `plugin_manager.cpp` calls) makes the call. Host tables are byte buffers with a marker in each optional slot. It covers:

- a loader that never calls the export (and an export given a null or short struct): the level stays at the declared need and every accessor returns `nullptr`;
- the call passes the struct's size and the loader's own level (`PLUGIN_INTERFACE_VERSION_MAX`);
- after the call every accessor returns the member in its slot, and a lower reported level turns on only the members it has;
- a fault in the export makes the call return false and the filter sees the exception.

```
msbuild Tests\PluginHostInfoTests\PluginHostInfoTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\plugin_host_info_tests.exe
```

It prints one line per failed check and a count, and exits 0 when every check passes.

## KeyAndThreadTests

A console program like PanelUnloadTests, for the two queries interface v72 gives a plugin's tick:
`IPluginInputEvents::IsKeyHeld` and `IPluginEngineEvents::IsGameThread`. It compiles the loader's
own `hooks/input/keybind_registry.cpp` and `utils/game_thread_dispatch.cpp`. The keyboard and the
foreground window are test doubles: the project's `pch.h` stands in for the loader's and points
`GetAsyncKeyState`, `GetForegroundWindow` and `GetSystemMetrics` at `test_doubles.cpp`, so the test holds keys and moves
focus without touching the real desktop. The two text-field checks the typing gate makes (a focused
game text widget, an active ImGui text field) are doubles too, and key messages go straight to the
registry's `ProcessWindowMessage`, as the overlay's window procedure hands them over. It covers:

- `IsGameThread`: false on every thread before the first tick, the one that will tick included;
  after a `Drain`, true on that thread and in a task posted to it, false on any other.
- `IsKeyHeld`: single keys, case-insensitive names, modifier chords, `Shift+K` while Ctrl is held as
  well (false: a name with modifiers needs exactly those, as its keybind callbacks do), a bare name
  while modifiers are held (true), a modifier bound as a plain key, mouse buttons, the
  layout-dependent `Tilde`, swapped mouse buttons (the logical button counts, as for the callbacks).
- `IsKeyHeld` is false for a name with no known key (unknown, empty, modifiers only) with every key
  down, when another window or no window has focus, and before the game window exists. It reads a
  name as `RegisterKeybindByName` does: `Ctlr+K` reads as K, and in `W+K` the last key wins. It makes no allocation, and it gives a
  consistent answer from four threads while a fifth moves the focus.
- `IsKeyHeld` and text fields, the keybind dispatch's typing gate: a key pressed while a game or
  ImGui text field has focus is false until released, closing the field included; pressed again
  outside one, true. A key held before the field took focus stays true, through Windows' key
  repeats too, and a key-up the rebind modal swallows still ends a held-back press. Escape, a `SetTypingExempt`
  key and mouse buttons are true while typing. A key with a keybind gives the callback's answer on
  its first poll, and the callback still skips the press. A key with no keybind and no poll skips
  the game's text-focus scan.
- The InputKey hook's key lookup (`NameToModKey` and `ShouldBlock`, which the hook calls with the event's own key name): a key's name finds that key whatever else is held, names with no mod key (mouse wheel, gamepad buttons and axes) are never blocked, a plain wheel notch survives a blocked LeftControl, holding one blocked key does not block another key, and a combo blocks only under its own modifiers.
- `ForgetPluginBlocking`: an unloading plugin's blocking goes, another plugin's blocking of the same key stays, and a plugin whose name starts the same is left alone.

```
msbuild Tests\KeyAndThreadTests\KeyAndThreadTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\key_and_thread_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## HookBrokerTests

A console program like PanelUnloadTests, for the hook broker (`hooks/hook_broker.cpp`), cuts included.
It compiles the loader's own `hook_broker.cpp` and `hooks_common.cpp` and puts real prologue patches
on small functions it writes into its own pages; the logger is the only test double. After each
change it reads every thunk slot back: each one must go to the detour of the first later link that is
not cut, else to the trampoline, and the patched bytes must not have changed. It covers cutting and
restoring the first, middle, last and all links, links added before and during a cut, removing links
under every cut pattern, links with no module (never cut), and three threads calling a function while
the cuts are switched 10,000 times. It also checks that the head slot already leads to the trampoline
when a first patch is written, which it sees from the log line before the write. It ends itself after
two minutes if it hangs.

```
msbuild Tests\HookBrokerTests\HookBrokerTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\hook_broker_tests.exe
```

It prints each test's name and any failed check, and exits 0 when every check passes.

## VersionMapTests

A console program like LayoutCheckTests, for the loader's map keys (interface v72): a plugin's
`PLUGIN_SCAN_FLAG_MAP_KEY` request (`VersionMap::ResolveMapKey`), the loader's own registry rows
(`ResolveLoaderEntry`), the fingerprint a site's rows are kept under (`VersionMap::Fingerprint`)
and field requests (`VersionMap::SelectField`, with a stand-in for the game's reflection).
It compiles the loader's own `memory_scanner/version_map.cpp`, `scanner.cpp` and `image_info.cpp`.
The pattern scan, the scan cache, the game version and the log are test doubles
(`test_doubles.cpp`): a pattern "resolves" to the RVA its table gives it in the test exe.

`fixture/hooks/game/version_map.generated.h` is a small map in the format starrupture-ci's
`ci/version-map.mjs` writes (two builds, a handful of keys), and comes first on the include path, so
`version_map.cpp` reads it instead of the loader's. It covers:

- The fingerprint: the two test vectors (a plugin request without and with `FOLLOW_REL32`), the
  pattern read as `Scanner::ParsePattern` reads it, 0 for no pattern or one that does not parse,
  and the arithmetic revision.
- The loader's row for the running build first, before the plugin's own row for it; the plugin's
  own row for this very exe agrees with it, or the result is AMBIGUOUS.
- A global `absent` row refuses an owner it lists, with no fallback, and an unlisted owner falls back
  to its own pattern. A row of another kind is skipped, and a key-only request is then refused.
- Site keys: read as `<plugin>/<name>`, used only with the request's fingerprint; another
  fingerprint's row is skipped, and when it is for another build and the plugin's own pattern does
  not match, the result is a failure, not that row's address. A site key-only request is refused,
  another plugin's site is not found, and an `absent` row with the request's fingerprint refuses it
  even when its own pattern matches.
- A key the map does not hold: the own pattern, or with none a failure; a global key with a `/`
  reads nothing; a plugin named `ModLoader` is refused.
- Two loaded plugins with the same name: neither reads site keys, and a global key is unaffected.
- Fields: a native member only the map holds takes the map's row; an own row that agrees passes,
  one that disagrees for any build is refused, and one for a build the map lacks is used. A key
  naming another class or member is set aside; a row recording a rename is used. A map row that
  reflection disagrees with is refused. A native member past its class's reflected end is refused,
  one ending at it passes, one in a class reflection does not know is refused, and with no
  reflection reader a native member is refused.
- The loader's own site entry: `ModLoader/<name>`, its fingerprint, and a row with a pattern other
  than the site's own is not used.
- A game build the version map has no rows for (CL-130045 against the two-build fixture): the newest build's
  row is rescanned on this exe and an older one is not read; a pattern not found, found twice or
  disagreeing with the request's own gives no address, and a recorded RVA is never used.
- The unknown-build fallback (`core/unknown_build.cpp`): the decision table (no CL, switched off,
  guarded, retry), the session id (it changes with the image stamp, the image size, the loader build
  and the CL), the newest known CL, the `modloader.ini` round trip on a temporary file (arming clears
  `Retry` and writes `Session`, a clean end clears it, a second end does nothing and a file that
  cannot be written arms nothing), and the player text: every "map" reads "version object map" first
  and "version map" after, both CLs are named, and the guard dialog says how to retry.
- The loader's real map (`hooks/game/version_map.generated.h`) against its registry: format 2, every
  key a registry entry's, every row with that entry's kind and fingerprint. It also checks the real
  rows the image library reads (the six texture chain rows and the two heap rows, on the game builds
  that have them), the hotbar rows and the close-menu key. The loader's lookup of those rows is served
  on both builds with reflection answering as the game does: `UTexture` is a class and bounds its
  member, and `FTexture`, whose name the class carries too, is a plain type with no class bound.

```
msbuild Tests\VersionMapTests\VersionMapTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\version_map_tests.exe
```

It prints each failed check and a count, and exits 0 when every check passes.

The same project builds a second program with `/p:BuiltFor130045=true`: `plugin_class_tests.exe`,
the loader's sources against `fixture130045\` (the fixture map with `kBuiltCL` 130045). Both
programs run `plugin_class_matrix.h`: every kind of released plugin (no tail, built for 127004,
migrated or not, built for 130045) on a running 127004 and a running 130045. The loader made for
127004 (v1.26.1's build) and the one made for 130045 must admit and turn away the same plugins.

```
msbuild Tests\VersionMapTests\VersionMapTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64 /p:BuiltFor130045=true
build\tests\plugin_class_tests.exe
```

The Server configurations build the server table's test instead (`server_map_tests.cpp`): the real
`hooks/game/version_map.server.h` (its function, field and size rows: counts, order, and the server's own
values) against the five places `version_map.cpp` serves it, with `MODLOADER_SERVER_BUILD` defined. Every
function, member and size row is asked for on the server exe and must come back with its value, and on
any other exe must not. The four Game-SDK globals' registry rows are checked against their server rows the
same way, along with `VersionMap::ServesExe` and what plugins are handed on the server.

```
msbuild Tests\VersionMapTests\VersionMapTests.vcxproj /p:Configuration="Server Release" /p:Platform=x64
build\tests\version_map_server_tests.exe
```

## VersionFeedTests

The signed feed reader (`memory_scanner/version_feed.cpp`, with the vendored Monocypher) against the
fixture map of VersionMapTests, with no network. It checks:

- Verify, then parse: a valid file, a flipped byte, an unknown key ID, a truncated, oversize or
  doubled file, another `format`, a bad `seq`, a scoped row with no `fp`.
- A file signed by Node's crypto with the TEST key verifies against slot `k1`. Built with `/p:VmapReleaseStyle=true` (no `VMAP_TRUST_TEST_KEY`, as a release is), the suite checks that only the release key `k2` is trusted and that the TEST-key feed is refused.
- The merge: a clash on the running exe keeps the baseline, a feed locator fills a baseline `absent`,
  an `absent` for one `fp` and a locator for another both survive, a rebuilt exe of a known CL.
- The per-CL coverage gate, counting a `ModLoader/` entry only under the loader's own `fp`.
- The cache: replaced only by a higher `seq`, through a temp file; a damaged or dropped-key cache is
  ignored; one map generation per process.

```
msbuild Tests\VersionFeedTests\VersionFeedTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\version_feed_tests.exe
```

## FeedFetchTests

The feed fetch (`auto_update/feed_fetch.cpp`) and the `HttpGet` under it (`auto_update/http_get.cpp`),
against a server on 127.0.0.1 that the test starts. The test switch `allowPlainHttp` lets it use
`http://`; every real request is HTTPS. It checks:

- No request (`WhyNot`): the server build, `[AutoUpdate] Enabled` off, no URL, no key. The server's hit
  count stays 0 through Refresh, Stage 1, Stage 3 and the refusal path.
- The stages: Stage 1 asks at every start and, with a cache whose ETag it holds, gets a 304 and changes nothing;
  a newer feed is stored and logged as one plain line; a 404 leaves the cache and its ETag as they were;
  Stage 3 asks once when nothing asked yet this launch and nothing after a request went out; a refusal
  after Stage 1 asked does not ask again; Stage 1 asks only with the main thread parked.
- The ETag: the one the server answers with is kept beside the cache with its `seq`; the next request
  sends it (`If-None-Match`) and a 304 is unchanged; an ETag kept for another `seq`, one with a control
  character, none on file, and a server that sends none each fall back to asking for the whole feed.
- The answers: a verified feed is stored verbatim; the same `seq` is unchanged; a 404, a server that never
  answers, a body that never arrives, a body over 1 MiB and a bad signature each fail and leave the cache
  as it was; a body that trickles in is cut off at the wall-clock deadline; `maxBytes` stops a longer
  body, reports that it was over the limit, and passes one exactly at the limit; an HTTP-to-HTTP redirect is followed.
- An unroutable host (192.0.2.1) fails inside the deadline plus one timeout.
- Release notes per version (`auto_update/version_compare.h`): `SelectVersionedNotes` lists the `## <tag>` sections
  newer than the previous version (two from 1.22.1, one from 1.23.0, none when previous is at or above all, the current
  version only when previous is unknown), with odd or missing headers, CRLF, empty sections and rc labels;
  `NotesForUpdate` is the window row for loader and plugin manifests (falls back to `notes` when the field is absent or
  selects nothing); `PreviousFromUpdateLine`, `PreviousFromInstalledLine` and `ResolvePreviousVersion` read the previous
  loader version from the pending value, the shown record or the updater's log; `DecideLoaderNotes` gates it all.
  A run of launches (quit before the main menu, two such quits, shown then relaunch, a newer release arriving while
  one is unshown, a stale pending value) checks that the notes and their history survive until the window opens.

Not covered here: a request to a real release, and the HTTPS-to-HTTP redirect refusal (that needs a TLS
server).

```
msbuild Tests\FeedFetchTests\FeedFetchTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\feed_fetch_tests.exe
```

It takes about 25 seconds, most of it the three cases that wait out a timeout.

## IconCopyTests

The bookkeeping behind `IPluginImGuiTextures::LoadFromUTexture2D` (`UI/icon_copy.h`), with the COM
`Release` and the device's `Evict` as a counting fake, so no GPU is needed. It checks:

- At most 8 copies start in one engine tick without waiting and at most 8 are on the GPU at once; past
  either limit a copy waits for the oldest one in flight instead of being refused. A new tick has a new
  budget, and a finished copy frees its entry for the next.
- A copy's references (the source, its heap, the new texture) are each released once its fence has
  passed, and the resident pageable (the heap, or a committed source) is evicted once. A lost device
  (every fence at `UINT64_MAX`) releases them all.
- Every early return, a throw (all texture slots in use) and a partial hold give back what was taken;
  a hold handed to the ring is released only by the ring; an abandoned one is never released.
- The protection choice: full with the version map rows (the heap or the resource is made resident),
  reduced with no rows or a heap pointer that does not check out (the copy still runs, and the source
  itself is still made resident).
- The device check on the engine's texture (`VtableIsFrom`): a resource whose virtual table is not in the
  module of a D3D12 resource the loader made itself is refused before anything is called on it, and so
  are a null table and an unplaceable one.

```
msbuild Tests\IconCopyTests\IconCopyTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\icon_copy_tests.exe
```

It prints one line per test and exits 0 when every check passes.


## ImageLibraryTests

The image library table (`UI/image_library.cpp`): ids, states and the caps, with no GPU and no engine.
It checks:

- Names are normalised, ids are stable and never zero, and the table stops handing out ids when it is
  full. A request keeps the name it was asked with.
- The states follow the read (NONE, WAITING, READY, NO_IMAGE); asking for the state does not mark an
  image as seen.
- The order requests are served in: images seen lately first, then set members; requests nobody drew
  are dropped.
- The byte cap and the count cap evict the least recently seen image, equal stamps evict the lower id
  first, a copy over the cap is refused, and an evicted image is asked for again when it is drawn. An image
  seen within the last half second is never evicted.
- `RoomFor` counts the free slots under both caps plus the kept images not seen lately, and `HasQueued`
  tells whether any image is waiting to be read.
- Handing out an id (`Item`, `Path`) asks for nothing: only drawing it does.
- Aliased ids are one image, `Canonical` gives the one id for them, `ReleaseAll` hands back every kept
  copy, and the plugin interface reaches the same table.
- Readers on another thread see a consistent state while the table changes.

```
msbuild Tests\ImageLibraryTests\ImageLibraryTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_library_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## ImagePumpTests

The image pump (`UI/image_pump.cpp`), which reads, shrinks and publishes the images the library was
asked for, against a fake host and a fake item list, so no GPU and no engine are needed. It checks:

- Nothing happens before the GPU is up; the caps follow the video budget.
- A started read is published when its copy is done; the shrink makes the stored size and counts the
  levels; the temporary copy is let go once the shrink starts; a shrink with no room is tried again, and
  one that fails ends as NONE.
- Not yet and busy are requeued (a not-yet image is given up on after 10 seconds), a path not in memory is
  loaded then read, and the per-tick limits on reads, requests looked at and package loads (one in all, items
  and sets included) hold.
- A preload image whose file fails goes back to preload, not into the queue for drawn images.
- A caught engine fault, or a copy the backend says is unavailable, turns the game's reads off once with one log
  line and no call back into the engine; the images that have a file are still served and the rest end as NONE.
- Newcomers are admitted only as far as there is room: images drawn lately are never cycled out for them, and
  while there is no room nothing is read and no file is loaded for the ones waiting.
- Items are held until the item list knows them, then follow the icon's path. An item with no icon, a
  placeholder icon or a placeholder path ends as NO_IMAGE; an item the game does not have ends as NONE.
- Registering an image reads nothing; only drawing it does. The item and building sets are listed only
  while a mod uses them and the setting is on, then preloaded one image at a time. A drawn image jumps
  the queue, and preload pauses for the setting, a waiting image, game-held memory, video memory and the
  caps.
- Evicted copies are freed on the next tick, a lost GPU drops everything without freeing, and shutdown
  frees what the library keeps.
- Nothing is logged per tick.
- The disk cache, against a fake source: a file hit never calls the host's load or read, a file that is
  rejected or cannot be uploaded falls back to the game and is saved again, nothing is served while the
  folder is being listed, saved names tie an item to its texture without building the item index, a saved
  set list fills the set without loading a package, and files are not held back by the game's own limit.

```
msbuild Tests\ImagePumpTests\ImagePumpTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_pump_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## ImageLookupTests

The rules behind the image lookups (`UI/image_lookup.cpp`), against a fake engine, so no game is needed.
It checks:

- Which textures are placeholders (the engine's white stand-ins), in every form a path comes in, and
  that a texture with the word only in its name is not.
- The item index: it is built in steps within a time budget, finds an item by its unique name or by the
  name of its data asset (also as the class name or the default object's name; a unique name wins, and an
  asset name answers only once every item is read), never
  finds an unset name ("None"), counts the items with no unique name, and reports when it is done or could
  not be built.
- `FromTexture`: it works on the game thread only, answers from a bounded memory of textures it has seen,
  and checks a texture's slot before it trusts it.

```
msbuild Tests\ImageLookupTests\ImageLookupTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_lookup_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## ImageDiskTests

The files the image library saves (`UI/image_cache_file`, `image_cache_writer`, `image_disk`), with no GPU
and no game. It checks:

- An image file and a set list read back as they were written, for every shape the shrink makes.
- A file with a bad magic, another revision, a truncated or lengthened body, a changed byte, or a header
  that disagrees with itself (too many levels, a size out of range) is refused.
- The folder name changes when the game build, the pak set, the format revision or the stored size changes,
  and the pak set hash does not depend on the order the paks are listed in. The hash covers each pak's path,
  mount point, read order and file count, and the size and last-write time of the pak, its `.utoc` and its
  `.ucas`.
- The session's pak set: a set that cannot be listed, or a pak file that cannot be read, switches the disk
  part off with one log line and never gives a partial key; a changed set switches it off for the rest of the
  session, and a job queued under the old key is not filed. The set is looked at once in each tick that files
  an image, and the game thread reads no file stamp.
- The writer, on a real temporary folder: the folders are made, no temporary file is left, a file of the
  same size is left alone, a folder that cannot be written (write protected, full, closed to new files)
  switches the disk part off with one log line, a file another program holds open is retried and then
  skipped without switching it off, the queue is bounded, a job with no folder is dropped, a queued clear
  survives a switch-off, and nothing is written after stop.
- The saver, on a fake readback: a finished copy becomes a file, a copy with no readback or a lost one makes
  none, a full queue drops the image, and the set list and the name list are written when they change and
  again after a clear. Textures the game makes while it runs (`/Engine/Transient`, `/Memory/`) are not saved
  and are left out of both lists.
- The name list reads back as it was written, and a bad one is refused.
- The reader (`UI/image_cache_reader`), on a real temporary folder and a fake uploader: it lists the folder and
  its two lists; a good file becomes a texture of the file's own bytes; a corrupt file, a file of another
  image or one of the wrong size is rejected, deleted and counted; an upload that fails keeps the file; a
  file gone after the listing fails; an off session reads nothing; and stop keeps what was finished. A file
  served from disk gets a new last-write time (so the size cap counts it as used), and one that could not be
  served does not. A file another program holds open is not deleted, a run-time object's file is not served
  and is deleted, and a session with no folder lists nothing.

```
msbuild Tests\ImageDiskTests\ImageDiskTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_disk_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## ImageCacheCareTests

The housekeeping of the image cache folder (`UI/image_cache_care`), on a real temporary folder, with no GPU
and no game. It checks:

- The current folder and the newest other one are kept and the rest of the root goes, with its set list; a
  current folder that is not made yet keeps one other.
- The 256 MB cap deletes the least recently used `*.img` files, the older folder before the current one, and
  never the set list. A file that is written again or used again counts as new.
- Temp files of game instances that are gone are removed; one of a running instance, one with another name and
  one that is held open are left alone.
- Clear empties the root and keeps the root; a file another instance holds open is skipped without an error.
- The tidy and clear jobs run on the writer's worker, not on the calling thread.
- Tidy with no current folder (the session's disk part is off) deletes nothing, and the clear job is still
  taken, and empties the root, after the writer has switched itself off.

```
msbuild Tests\ImageCacheCareTests\ImageCacheCareTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_cache_care_tests.exe
```

It prints one line per test and exits 0 when every check passes.

## ModIndexTests

The mod index behind the Browse tab (`auto_update/mod_index.h`, `mod_index_fetcher.cpp`), with no network and
no game. It checks:

- The parser on a good index, a missing optional field, an empty list, a list with bad objects (skipped and
  counted), text that is not an array, malformed or cut off, over the 256 KB cap, past 200 entries, over the
  field caps, and hostile text (deep nesting, braces in strings, NUL bytes, unterminated strings).
- The page URL allowlist: https on nexusmods.com, www.nexusmods.com, github.com and www.github.com only (a YouTube
  address is refused), and look-alike hosts, user info, ports, backslashes, control characters and other
  schemes refused.
- The README link allowlist (`IsAllowedLinkUrl`): the same hosts plus youtube.com, www.youtube.com,
  m.youtube.com and youtu.be, with the same refusals.
- `IsInstalled` by id or name, in any case.
- The sample `mod-index/index.json` parses with none skipped, lists the install fields on every entry
  except the two server-only ones, and sets `server_only` on exactly those two.
- The install fields: `download_url`, `download_sha256`, `download_size`, `install_files` (either order, with
  or without the `.json`) and `update_manifest_url`. An entry with a field missing or wrong is still listed,
  with nothing to install, and the old fields read as before.
- `server_only`: true only for a literal `true`; absent or anything else is false. A `server_only` entry that
  also carries the install fields reads with nothing to install.
- The list failure texts: HTTP 404 ("The mod list isn't published yet."), any other status ("The mod list
  server answered with an error (HTTP 503)."), a download over 1 MB ("The mod list is larger than allowed."),
  and no answer, which keeps the connection text. A list over 256 KB that did download is refused by the
  parser with "The mod list is too large to read.". Checked for the built-in address and for `ModIndexUrl`.
- The Browse search: names, authors and descriptions in any case, several words in any order, extra spaces
  and tabs, a word of 3 letters or more whose letters follow in the name ("minmap" finds MiniMap), and no
  match for words under 3 letters, letters out of order or letters scattered through a description.
- SHA-256 of a known text.
- The download, with a stand-in for the network: the size cap, the file, one redirect and then the file, the
  signed query passed on as it came, a wrong first address never asked for, redirects to another host, to
  http, with a port or user name, a relative one, a later hop, and a fourth redirect refused. A wrong hash, a
  short file, a long file, an interrupted answer, no answer and any other status fail with a reason. Cancelling
  before or during stops it. `HttpGet` is a stand-in here, so the test checks the options the loader passes
  to it (`noRedirect`, no plain http) and not what WinHTTP does with them; a body over the size in the mod
  list fails as the wrong-file case ("The downloaded file does not match the mod list, so nothing was installed.").
- Writing the files into a temp folder: the DLL and `.json` land with no temp file left, an existing `.json`
  or DLL is refused and named, a file that appears before the rename is left alone and nothing of ours stays,
  a failed rename cleans up, and a Vortex-listed mod is refused with the same words before the download and
  at the write; a `X.json` left over from our own earlier install (the DLL gone) does not refuse and is
  replaced, while one that is not ours still does; a rename that hits a sharing violation is tried again
  a few times, and a name that is simply taken is not tried again.
- The requirement messages ("MainMap needs MiniMap. Install MiniMap first.", "Enable MiniMap first.").
- The installer: one install at a time, the outcome names what was placed, nothing starts after Shutdown
  (`Halted()` is false before it), and a Shutdown during a download writes nothing and calls nobody back.
- Whether a list was signed: the loader's own address is, a bare list from `ModIndexUrl` is not, a signed
  file from `ModIndexUrl` is.
- The Fetcher's states (Idle, Loading, Ready, Failed) through an injected request: one request however often
  the first load is asked for, Retry from Failed or an empty list, Shutdown, and a worker that outlives its
  Fetcher. `HttpGet` is a stand-in the test defines, which also shows the request passes the feed's timeouts
  and the index size cap.

```
msbuild Tests\ModIndexTests\ModIndexTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\mod_index_tests.exe
```

It prints one line per failed check and a total, and exits 0 when every check passes. The last run was
`585 checks, 0 failed`.

The mod index builder has its own tests (`node --test .github/vmap/build-mod-index.test.mjs`, 14 of 14): the
install fields it accepts and refuses, exactly one plugin `.dll` in `install_files`, an install field that is
repeated or nested, a size not written as plain digits, `server_only` that is not `true` or `false`, and
`server_only` together with install fields.

## PluginManageTests

Disabling and uninstalling a plugin, with no game. It has two parts, and every case works in its own folder under
the temp directory.

The rules (`plugins/plugin_manage.h`):

- The `[Plugins] Disabled=` list: splitting, adding and removing a name in any case, and saving it to
  `modloader.ini` without touching the other sections or leaving a half-written line.
- Which names are refused: paths, `..`, colons, anything that is not a plain `<name>.dll`.
  Junctions and linked files are refused too. The linked-file case is skipped, and says so, when the account may
  not create a file link.
- Uninstall moves only `X.dll`, `X.pdb` and `X.json` into `Removed\X\<stamp>`. Settings, subfolders and
  neighbouring files stay.
- Rollback when the move of file 1, 2 or 3 fails, in both directions, and what is reported when the rollback
  fails as well.
- Restore: the newest backup, refusal when a file is already back, and a `removed.ini` that was edited by hand.
- A DLL loaded in the test process can be moved out of the folder and back.
- The names a preload DLL answers to, the card states and buttons, and the "Vortex placed this file" check.

This part ends with a scripted run (disable, enable, uninstall, restore) that prints each step and the files after
it.

The plugin manager (`manager_tests.cpp`) runs the loader's own `plugin_manager.cpp` against small test plugin DLLs
(`ManageTestPlugin.vcxproj`, built along with the tests). The preload step, the network and the other loader parts
it calls are stand-ins, and work meant for the game thread runs on the caller. There are 9 made-up game folders
and 15 game starts, each start a process of its own, so a restart is a real one. It checks:

- Disable, uninstall and restore through the same calls the Browse tab makes, which plugin code runs after each,
  and that file contents do not change.
- A plugin that cannot unload keeps running until the restart.
- The auto-updater's sidecar scan skips disabled and uninstalled plugins, and a `.json` left without its DLL.
- Recovery after a rollback that could not finish.
- Restore leaves a disabled plugin disabled, and is refused when the same plugin is installed under another file
  name.
- A DLL copied back in the same session is picked up by LOAD NEW.
- A plugin Vortex placed is not uninstalled by the loader, read from a real `vortex.deployment.json`.
- Install, through the call the Browse tab makes, with the download a stand-in (`install` step, 48 checks): a
  list that isn't signed installs nothing, whether it comes from the built-in address or from `ModIndexUrl`; a DLL that is already
  there is refused before any download; a missing or disabled requirement is named; a bad hash or a failed
  download leaves no file and no temp file, and says why; a plugin installed after an uninstall loads at once
  (`Done`), clears its Disabled entry and records the entry's version for the auto-updater; a plugin whose old
  code is still loaded installs as "after restart" without loading its code twice; a `server_only` entry is
  refused even when it carries the install fields; and a second install while one runs is refused with
  "Another mod is installing. Try again when it finishes." (the first install is held in its download while
  the second is asked for).
- While a plugin call has not returned, UNLOAD, RELOAD and Disable change nothing and ask for a retry (the
  unload notes never name RELOAD), a plugin disabled that way is refused a RELOAD with its reason in the plugin
  list, a plugin that cannot unload gets that reason instead, and UNLOAD works once
  the call returns. No test times out the wait that follows a plugin's shutdown through the plugin manager.
- What this step does not reach: in the tests `PostVoid` runs the task inline (`manager_stubs.cpp`), so the
  hop to the game thread, the lock order and the posted task's `Halted()` check are not exercised. Those
  are checked by reading the code and in the game.

```
msbuild Tests\PluginManageTests\PluginManageTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\plugin_manage_tests.exe
```

It prints one line per failed check and a total, and exits 0 when every check passes. The last run was
`656 checks, 0 failed`.

## ImageShrinkTests

The size, level and byte rules of the shrink (`UI/image_shrink.h`), with no GPU. It checks that a larger
image is scaled to the stored size with its proportions kept, a smaller one keeps its own size, a side never
shrinks to nothing, the level chain runs down to one pixel, and the bytes count every level.

```
msbuild Tests\ImageShrinkTests\ImageShrinkTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_shrink_tests.exe
```

## ImageShrinkHarness

The shrink pass itself (`UI/image_shrink_gpu.cpp`) on a desktop D3D12 device, with no game, window or swap
chain. Each case builds an image like the game's (BC3, BGRA or RGBA16F, with or without smaller levels),
copies the level the loader would copy, shrinks it, reads the result back and checks the size, the number
of levels and the colour on every level, and times the pass on the GPU.

It also records the readback of the stored copy (`UI/image_readback_gpu.cpp`) in the same command list and
checks that the bytes of every level equal the pixels the shrink produced, and that the readback ring gives
no ticket when full and gives a slot back when it is fetched or cancelled, including after a failed signal.
An sRGB source is shrunk through a raw (non-sRGB) view of its bytes, so a mid grey is stored as it was, not
brightened.

Last, it uploads pixels laid out as a saved file holds them (`UI/image_upload_gpu.cpp`), reads the texture
back and checks that every level equals the bytes that went in.

The run ends with the debug layer's error count. It needs the Windows Graphics Tools feature; without it
the harness says the debug layer is not available and exits 1 even though its checks passed.

```
msbuild Tests\ImageShrinkHarness\ImageShrinkHarness.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\image_shrink_harness.exe
```

## CommandLineLogTests

A console program that includes the loader's own `core/command_line_log.h`, which both the loader
and the proxy use to log the game's command line. It checks that only the program's file name and
the switch names come out (`-Key=Value`, `-Key:Value`, `-Key Value`, quoted values, a URL
argument), never a value or a directory.

```
msbuild Tests\CommandLineLogTests\CommandLineLogTests.vcxproj /p:Configuration="Client Release" /p:Platform=x64
build\tests\command_line_log_tests.exe
```

It prints the number of checks and how many failed, and exits 0 when none did.
