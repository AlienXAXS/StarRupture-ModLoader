# Version map

The version map says where the game's functions and class members are on each game build. A new
game build needs new rows in the map and nothing else: no loader release and no plugin release, as
long as plugins take their game data from the map.

For plugin authors, the request side is in the
[SDK guide](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md),
and [From fingerprinting to mapping](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/FingerprintToMapping.md)
is the short step-by-step. For players, see the README's [Game updates](README.md#game-updates).

## What the map holds

One row per key and game build (a build is its CL, plus the exe's PE time stamp and image size).

| Row | Key | Holds |
|---|---|---|
| function | `Class::Function` or a global's name (`GWorld`) | A pattern, `resultOffset`, `followRel32At` and `followRipDispAt` as in a `PluginScanRequest`, the `rva` CI verified, and the `kind` it was verified with |
| function (site) | `<owner>/<hookName>` | The same, plus `fp` |
| data | a global's name | A function row of kind `DATA` |
| field | `Class::Member` | `offset`, `size`, a bool mask, and the member's new name if it was renamed |
| absent | any function key | No locator: CI could not place the key on that build |

**Key.** A key names a function, a global or a member in Dumper-7 naming. A global key (kinds
`FUNCTION_START` and `DATA`) names one game symbol, the same for every plugin, and has no `/`. A
site key (kinds `IN_FUNCTION`, `CODE`, `VTABLE`, `ANY`) names no symbol. It is
`<owner>/<hookName>`, where the owner is the plugin's `PluginInfo` name, or `ModLoader` for the
loader's own. Only that owner reads it.

**Kind.** A row is used only for a request of the same kind. Otherwise it is skipped with a log
line.

**fp.** A site's address is right only for the request it was verified with. A site row carries
`fp`, an FNV-1a 32 fingerprint of the request (kind, pattern, `resultOffset`, `followRel32At`,
`followRipDispAt`). The loader uses the row only when the incoming request has the same `fp`. A site
row with no `fp` matches nothing. Global and field rows carry none.

**Absent.** An `absent` row records that CI tried and could not place the key on that build. The
requests it binds fail with no fallback. A verified row replaces an `absent` row; nothing else does.

The map lives in `hooks/game/version_map.generated.h`, written by CI. Every loader build works
offline on every build in it. A signed feed (below) adds to that.

**Not map data.** Game enum numbers are read by name from the game's reflection data
(`PluginMap::EnumValue`). Unreal Engine constants (`RF_*` bits, `INDEX_NONE`, the `TArray` header)
are fixed by the engine version, not by a game build.

## Selection

A plugin asks for a function with `PLUGIN_SCAN_FLAG_MAP_KEY` and for a field with a field request.
For a function key the order is:

1. **The loader's row for the running CL.** On the row's own exe (CL, stamp and size all equal) the
   result must equal the row's `rva`; a mismatch fails with no fallback. On another exe of the same
   CL the row's pattern is tried, and if it is not found the row is skipped with a warning. A site
   row must also have the request's `fp`, or it is skipped.
2. **The plugin's own row for the running CL** (`candidates`, or its pattern on its built CL). When
   it is for the running exe, its `rva` must equal the loader's pick, or the result is AMBIGUOUS.
3. **Agreement** over every remaining locator: the plugin's pattern, the plugin's rows for other
   CLs, and, when there is no loader row for the running CL, the row of the latest CL the map has
   (rescanned on this build; it counts only if its pattern resolves uniquely and as the request's
   kind). They must all resolve to one address. Older rows are not read.
4. Otherwise the request fails. A declared feature turns off; any other key refuses the plugin.

An `absent` row for the running CL ends the search for the requests it binds. There is no by-name
step for functions: a UFUNCTION's name leads to its exec thunk, not the native function a hook needs.

**The plugin's own pattern.** It has to match the running exe exactly once, even when a row decides
the request. If it matches 0 or 2 or more places, the loader logs one warning
(`[Map] <plugin>: <key>: its own pattern matches <n> times on this exe ...`) and still uses the row.
Where no row resolves the key (a loader with no map, a dedicated server), such a pattern is refused.

**AMBIGUOUS.** Two locators for the same build resolve to different addresses. The loader picks
neither and the log lists each locator and where it landed.

**Shared names.** A plugin may not be named `ModLoader`. A plugin whose name contains a `/`, or that
shares its name with another loaded plugin, reads no site keys.

A key that is not in the map falls back to the plugin's own row and pattern, logged as `not in the
loader's map; own pattern`. `DisablePluginVersionMap=1` under `[Debug]` in `modloader.ini` makes
every key behave that way for plugins, to test their own locators.

**Fields.** The map's row for the running CL first (else the latest CL's), then the plugin's, then
`builtOffset` on its `builtCL`. Two rows for one CL that disagree fail the request.

- A reflected field is read live from reflection. A row is only a cross-check: where they disagree
  the game wins and the difference is logged.
- A native offset (reflection does not describe it) comes from the running CL's row or the compiled
  offset on the build it was compiled for. A row from an older CL cannot be checked on this build,
  so the field is unresolved. The offset plus size must lie inside the class's live size.
- `PLUGIN_FIELD_ANY_SIZE` (size 0) returns whatever size the build has. For a native member it needs
  the running build's own row.
- A plain C++ engine type that reflection does not know (`FMassEntityManager`) is served from the
  running CL's row alone, with no class bound.

**Struct sizes.** A size request is a field request with the member null. Its row is a class-keyed
field row: the class name alone, offset 0, the struct's size, mask 0.

- A reflected struct is answered from the game's own reflection, so each build gives its own size.
  The row is a cross-check only.
- A struct the game does not reflect is served from the running build's own row. With no such row
  the request is refused.
- `PLUGIN_FIELD_CHECK` is for code that depends on the size (a struct copied by value, a
  `ProcessEvent` parameter block). Size 0 with no flag is for code that only steps through an array.
- `ModLoader\compat-report-<CL>.json` lists every size a plugin requested, with the row's size next
  to the reflected one.

**Function parameters.** A UFunction is a reflected struct whose members are its parameters. The
class of the request is `<Class>_<Function>` with the game's class name and no prefix letter
(`CrEnviroWaveSubsystem_GetCurrentStageSettings`), or `/Script/<Module>.<Class>_<Function>`.
`SR_PARAM` is a member request (`PLUGIN_FIELD_ANY_SIZE`; `Param` is `ReturnValue` for the return)
and `SR_PARAMS_SIZE` is the size request, which gives the function's `ParmsSize`. Both are answered
live and need no row. A name that matches no function, or more than one, is refused with the reason;
the `/Script/<Module>.` form picks one. Reads are on the game thread.

A Blueprint class (the name ends in `_C`) is normally not loaded when plugins load. `SR_PARAM` and
`SR_PARAMS_SIZE` carry `PLUGIN_FIELD_LATE` for it: a request whose class is not loaded is **pending**,
not a failure.

- The log says `pending` once, the compat report lists the key under "Waiting for a game class to
  load", and the feature that lists the key is off and greyed in its config. The plugin still loads.
- A key that no declared feature lists cannot wait and refuses the plugin like any miss.
- The plugin repeats the request from the game thread (`PluginMap::ResolveLateFields`) until it
  resolves, then switches its own feature on. The loader looks at most every two seconds per key,
  and never again once resolved. It copies the parameter names, offsets and sizes and keeps no
  object pointer.
- Only `CPF_Parm` properties count, so a Blueprint function's locals do not stretch the block size.
- An older loader ignores the flag and refuses a Blueprint class.

## Features

A plugin can group the keys one part of it needs into a feature, with a plain-English title and the
config entries it drives. How to declare one is in the
[SDK guide](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md#features----declarefeature).

- A key that fails and belongs to a declared feature turns that feature off. The plugin still loads.
- A key that fails and belongs to no feature refuses the plugin.
- The failure window lists the feature by its title. The log has `[Map] <plugin>: feature "<title>"
  is off: <keys> (<reason>)`.
- Its config entries are greyed out and their stored values are kept.

A plugin is still refused for an interface newer than the loader's, for no built CL, and for
missing core globals. See [Admission](#admission). The Plugins tab shows a plugin with features off
as `Loaded (N off)`.

## Admission

Each plugin is judged against the build it was compiled for. `PluginInfo` ends in a tail from
interface v72: `gameBuiltCL` (`PLUGIN_GAME_BUILT_CL`) and `mapFlags`.

- On its own built CL a plugin loads as it always did.
- On any other supported CL it is admitted only if it is interface 72 or later, sets
  `PLUGIN_MAP_MIGRATED`, and called `GetSdkGlobals` in `PluginInit`. Otherwise the Plugins tab shows
  it as **Incompatible** ("Needs an update to run on CL-..."), and it still loads on its own build.
- A v70 or v71 plugin has no tail and counts as built for CL-127004. So does a v72 or later plugin
  whose `gameBuiltCL` is 0, with a warning. A tail that sets `PLUGIN_MAP_MIGRATED` and names no build
  is refused on every build.
- An older interface is never refused for its age. Only an interface newer than the loader's is
  refused, as needing a newer mod loader. A plugin before v70 counts as built for a build before
  CL-127004, so it shows "Needs an update" and is never called.

`PLUGIN_MAP_MIGRATED` is the plugin's promise that it reads no game data from compiled-in offsets or
addresses (checklist in the SDK's guide). The loader does not check it.

## The feed

Between loader releases, the map can be refreshed from a signed file, so a new game build is
supported without a new loader.

- **Asset.** `version-map-client.v1.txt`, on the loader's Latest release.
- **When.** Only on the client with `[AutoUpdate] Enabled` on: at startup when the running exe is in
  neither the built-in map nor the cache (the splash shows "Downloading game data for CL-n"); after a
  version check or pattern preflight refuses the build, for the next launch; and from the auto-update
  thread, at most once a day.
- **Request.** One HTTPS GET with short timeouts. The body is capped at 1 MiB.
- **Cache.** `ModLoader\version-map-client.v1.cache`, verified again on every launch and replaced
  only by a file with a higher `seq`.
- **Signature.** The file is verified (Ed25519) before anything in it is read, and rejected whole if
  it is malformed, signed with an unknown key, or has a bad signature.
- **Merge.** The loader merges the built-in map and the feed once at startup. For each group it takes
  the built-in row for the running exe if there is one, otherwise the feed's. The feed can add builds
  and keys and fill a shipped `absent` row. It can never change a locator the loader shipped with for
  the running exe. A CL known only from the feed is supported only if the union holds a locator for
  every required loader entry and field on it.

A bad feed cannot run code. Every row still passes its kind check, the uniqueness rule, equality
with its `rva` on its own exe, and for a native field the class-size bound.

New game builds reach the feed through the CI in
[starrupture-ci](https://github.com/msantoro12/starrupture-ci); its `ci/VERSION-MAP.md` has the
feed format.

Maintaining the feed itself (the key slots, signature checking, the feed URL and publishing) is in
[MAINTAINERS.md](MAINTAINERS.md).

## Getting a value added to the shared map

A plugin can carry its own row for a key (the SDK's `plugin_version_map.h`), and the loader uses the
shared row first and the plugin's own row second. To have the pipeline keep a value current on every
game build, send a PR that adds it to the shared map and to the CI key list. The steps are in the
SDK's [From fingerprinting to mapping](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/FingerprintToMapping.md#3-get-the-ci-row-for-each-key).
In short:

1. **A function or global.** Add the plugin's rows for the build you tested to
   `hooks/game/version_map.generated.h` (`kFunctionRows`), with the `rva` you verified, and add your
   plugin to the `plugins` input of starrupture-ci's `sr-client-check.yml`. A site key is
   `<PluginName>/<hookName>` and carries the request's `fp`.
2. **A native member, or the size of a plain C++ struct.** Add a row to `kFieldRows` for the build
   you tested, and a line to `ci/native-keys.txt` in starrupture-ci. Reflected members and structs
   need neither.

Row formats, from the generated header:

    // MiniMap/FBulkData::GetBulkDataSize
    { 127004, 0xE8972298, 0x0F35C000, 3, 0x26FFCBE0, false, "44 0F B6 49 0C ...", 0, -1, -1, 0x015BFD50, nullptr },
    // UCrPlayerMapMenuDataComponent::FOWData
    { 127004, 0x100, 0x10, 0x00, nullptr },

A function row is: CL, image stamp, image size, kind, `fp`, absent, pattern, `resultOffset`,
`followRel32At`, `followRipDispAt`, `rva`. For a build whose exe you have no PDB for, use `rva` 0:
the loader then finds the function by scanning the pattern (it must match exactly once, as the
request's kind). A field row is: CL, offset, size, bool mask.

A `native-keys.txt` line is `<plugin> <Class::Member>`, or `<plugin> <ClassName>` for a struct size.
After the key it may give `<offset> <size> <mask>` in hex (only when the PDB's size is not the one
the plugin was built with), then `optional` if the plugin runs without that key:

    MiniMap UTexture::SRGB optional
    MyPlugin FMyStruct

A key is optional only when every plugin's line for it says so. The header of the file has the full
format.

Before you send it, run the SDK's rule-3 check (`tools/rule3-check.mjs`), `Tests\VersionMapTests`
here, and `node --test ci/*.test.mjs` in starrupture-ci.

## Failure modes

What a player sees, and what the log says (`ModLoader\Logs\ModLoader.log`).

| Case | Result |
|---|---|
| The running build is in the map | Starts with no network request. |
| New build, no feed published yet, or no network | The loader refuses the build. The player gets the "Start Game Without ModLoader" dialog. It tries again at the next launch. |
| New build, feed published | The splash shows "Downloading game data for CL-n" for a moment, then the game starts modded. |
| A fetch fails (404, timeout, over 1 MiB, not https) | As if there were no feed: a known build runs, an unknown one is refused. |
| The feed's signature is bad, the key unknown or the file malformed | The file is discarded whole. Same as no feed. |
| The feed is older than the cache | Ignored. |
| `[AutoUpdate] Enabled` off, or a server build | No fetch. The built-in map is used. |
| A row is `absent` for something a plugin needs | That plugin's request fails. A declared feature turns off; otherwise the plugin is refused. Other plugins and the game run. |
| The feed has the build but misses a required loader entry | That CL is dropped from the feed, and the build is refused as above. |
| A plugin built for another build does not declare `PLUGIN_MAP_MIGRATED` | **Incompatible**: "Needs an update to run on CL-...". It loads on its own build. |
| A plugin's own pattern matches 0 or 2 or more places, but a row resolves the key | The plugin loads on the row, with one `[Map]` warning. Where no row resolves the key it would be refused, so the author should make the pattern unique. |
| Two locators for one request disagree (AMBIGUOUS) | The request fails, with the locators listed. |
| The game's `UObject` virtual table no longer holds `ProcessEvent` where the SDK calls it | Every plugin is switched off for the session, with the same dialog as a layout change. |
| A game class a plugin reads moved | That plugin's own field request, or its `SR_SIZE_CHECK`, fails and the plugin alone is affected. A plugin that keeps the game's layouts compiled in is not loaded; one that declares `PLUGIN_MAP_MIGRATED` still loads, and a request for the compiled offset of a changed class is refused, which turns off only the feature that asked. |

A fetched map is read once at startup and never changes mid-session.
