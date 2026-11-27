# Version map

How the mod loader finds game functions and game-class members across game builds, how a new
build reaches players without a loader release, and what each failure looks like. For plugin
authors the request side is in the
[SDK guide](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md);
for players the short version is the README's [Game updates](README.md#game-updates).

A game update moves functions and fields. The version object map (v map) is the one place that says where they
are on each build. It maps each game version's objects (functions, fields, sizes) for easy access.
So a new build means new rows in the version map and nothing else: no loader release
and no plugin release, as long as the plugins take their game data from the version map.

## What the version map holds

The version map has one row per key and game build (a build is its CL, plus the exe's PE time stamp and
image size). There are four kinds of row.

| Row | Key | Holds |
|---|---|---|
| function | `Class::Function` or a global's name (`GWorld`) | A pattern, `resultOffset`, `followRel32At` and `followRipDispAt` as in a `PluginScanRequest`, the `rva` CI verified, and the `kind` it was verified with |
| function (site) | `<owner>/<hookName>` | The same, plus `fp` |
| data | a global's name | A function row of kind `DATA` |
| field | `Class::Member` | `offset`, `size`, a bool mask, and the member's new name when it was renamed |
| absent | any function key | No locator: CI could not place the key on that build |
| absent (field) | `Class::Member` | A member the game build does not have. It is the `kAbsentFields` array in the generated header, which the loader reads; the version map file has no such line |

**Key.** A key names a function, a global or a member in Dumper-7 naming. A global key (kinds
`FUNCTION_START` and `DATA`) names one game symbol, the same for every plugin, and has no `/`. A
site key (kinds `IN_FUNCTION`, `CODE`, `VTABLE`, `ANY`) names no symbol. It is
`<owner>/<hookName>`, where the owner is the plugin's `PluginInfo` name or `ModLoader` for the
loader's own. Only that owner reads it.

**Kind.** A row is used only for a request of the same kind. Otherwise it is skipped with a log
line, so the version map can never weaken a kind check.

**fp.** A site's address is right only for the request it was verified with, and old plugin
builds stay in the field after a newer one re-anchors a site under the same name. So a site row
carries `fp`, an FNV-1a 32 fingerprint of the request: its kind, its pattern as the loader parses
it, `resultOffset`, `followRel32At`, `followRipDispAt` and, for the loader's own entries, a site
arithmetic revision. The loader computes `fp` for the incoming request the same way and uses the
row only when the two are equal. A site row with no `fp` matches nothing, and in a feed it
rejects the whole file. Global and field rows carry none.

**Absent.** An `absent` row records that CI tried and could not place the key on that build. For
a global key it lists the owners whose requests CI tested; for a site key it carries the `fp` it
tested. It binds only those requests: they fail with no fallback, because their locators were
found unsafe on that build. A verified row replaces an `absent` row; nothing else does.

An absent field is a member the game build does not have, for example a class that arrives in a
later build. A request for it on that build is answered "not in this game version" and nothing
else is tried, not even a row the plugin brings. That answer is not a failure: see
[Features](#features). The list is compiled into the loader (`kAbsentFields`); the version map
file cannot carry it.
On CL-127004 the hotbar members `UCrHotbarLocalPlayerSubsystem::CurrentHotbarIndex` and `::Hotbars`,
and the hotbar slot members `FHotbarInternalSlotData::SlotType` and `FHotbarArray::SlotsArray`, are listed
this way, so the feature that needs them is off there without a warning. A member whose row is just
missing, because the map is stale, still warns.

The version map lives in `hooks/game/version_map.generated.h`, written by CI
(`ci/version-map.mjs` in [starrupture-ci](https://github.com/msantoro12/starrupture-ci), format in
its `ci/VERSION-MAP.md`). Every loader build carries the version map as it stood when it was built, and
works offline on every build in it. The signed feed (below) adds to that.

**The build the loader is compiled for.** The loader compiles one Game-SDK per configuration, and
`VersionMap::kBuiltCL` is the CL of that SDK. The client is built against CL-130045 (`Client\build_cl.txt`
in the SDK) and the server against CL-127004 (`Server\build_cl.txt`); `Shared.props` reads the number
from the file and passes it as `MODLOADER_BUILT_CL`. The `kBuiltCL` line in `version_map.generated.h`
is the client's (CI keeps that line). A client build stops if that line differs from `Client\build_cl.txt`,
if the file is missing, or if the newest layout table (`core/layout_table_<CL>.generated.h`) is for another CL.
The server build has its own table, `core/layout_table_server_<CL>.generated.h`, with the same rule.
The loader carries one layout table for each build it supports (CL-127004 and CL-130045), and the start check
uses the one for the running build's own CL, or the newest when the build has none.
The client's built-in rows still list every build it has run on, CL-127004 included, so the same
files run on both. On CL-127004 the compiled offsets are not trusted, the Game-SDK globals are found
by pattern, and a member that sits elsewhere on that build is read from the version map or the game. In the loader's
layout table that is `UCrEnviroWaveTimerSubsystem::TimerActor` (0x90 on CL-127004, 0x98 from CL-128786)
and `ACrCrafter::CraftComponent` (0x6D8 and 0x6E0). The start check leaves both out, so they do not
count as a changed class. The server is compiled for CL-127004 and lists only that build, plus the builds a downloaded server file adds.

**Not version map data.** Some values a plugin needs are not in the version map on purpose.

- *Game enums.* The numbers in a game `UEnum` can move on a game update, and no symbol or pattern
  holds them, so CI could not generate a row. The value belongs to the game's reflection data, read
  by the enum's and the entry's name.
- *Unreal Engine constants.* `EObjectFlags` bits (`RF_*`), `FUNC_Native`, `INDEX_NONE` and the
  engine ABI the plugins compile in (the `TArray` header, shared-pointer reference controllers) are
  fixed by the engine version, not by a game build. A change of engine version regenerates the whole
  Game-SDK, so the version map has nothing to add. The one the version map can help check is the slot of
  `UObject::ProcessEvent` in the object's virtual table, which every plugin's SDK compiles in: at
  startup the layout check compares that slot with the address the version map resolved for the function,
  and a difference switches all plugins off like a changed layout.

## Selection

A plugin asks for a function with `PLUGIN_SCAN_FLAG_MAP_KEY` and for a field with a field
request. The order for a function key is:

1. **The loader's row for the running CL.** On the row's own exe (CL, stamp and size all equal)
   the result must equal the row's `rva`; a mismatch is a failure with no fallback. On another
   exe of the same CL the row's pattern is tried, and if it is not found the row is skipped with a
   warning and the other locators decide. A site row must also have the request's `fp`, or it is
   skipped.
2. **The plugin's own row for the running CL** (`candidates`, or its pattern on its built CL).
   When it is for the running exe, its `rva` must equal the loader's pick, or the result is
   AMBIGUOUS.
3. **Agreement** over every remaining locator: other CLs' rows from the loader, the plugin's
   rows, and its pattern. They must all resolve to one address.
4. Otherwise the request fails. What that costs the plugin depends on what it declared (below).

With no row for the running CL, the loader uses the row of the latest CL it has one for, and
rescans that row's pattern on this build: it counts only if the pattern resolves, uniquely and as
the kind the request asked for, and then it joins step 3 as one more locator, so it and the
plugin's own pattern and rows must agree. Older rows are not read. A row that does not resolve
here is not used. An address that was not found on this build is never used.

There is no by-name step for functions. For a native function the plugin's own pattern stands in
for it. A UFUNCTION's name leads to its exec thunk, a different function with a different calling
convention from the native one a hook or call needs, so using it would be a wrong address.

An `absent` row for the running CL ends the search for the requests it binds.

**The plugin's own pattern.** A plugin's own AOB pattern has to match the running exe exactly
once, whether or not a row decides the request. When a row (step 1 or the latest row's rescan)
resolves, the pattern is not needed to load, so the loader checks it on the side: once per pattern
per session, through `scan_cache.ini`, with the same rule as any scan. If it matches 0 or 2 or more
places the loader logs one warning, `[Map] <plugin>: <key>: its own pattern matches <n> times on this
exe ...`, and still uses the row. Without that check a pattern that is ambiguous would pass here and
be refused wherever no row resolves the key (a loader with no version map, a dedicated server for a key its
table lacks, a build none of the rows resolve on), since the pattern is then the only locator. A pattern that matches 0 times
gets its own wording: it is stale for this build, and the row is carrying it. The count of a pattern
that is not unique is kept in `scan_cache.ini` under the exe, so a later launch repeats the warning
without scanning again. One line per pattern text: two plugins that share a pattern get one line,
naming the first. The loader's own registry patterns are not checked this way; CI already holds
them to the same rule.

**AMBIGUOUS.** Two locators for the same build that resolve to different addresses. The loader
picks neither, fails the request, and the log lists each locator and where it landed
(`AMBIGUOUS: the locators that resolve disagree:`).

**Shared names.** A plugin may not be named `ModLoader`, and a plugin whose name contains a `/`
reads no site keys. When two loaded plugins share a name, neither reads site keys, because
`<owner>/` cannot tell them apart. Each of these fails the request with a log line saying why.

A key that is not in the version map at all (a third-party plugin, or a hook newer than the feed) falls
back to the plugin's own row and pattern, logged as `not in the loader's map; own pattern`.
To test a plugin's own locators, `DisablePluginVersionMap=1` under `[Debug]` in `modloader.ini` makes
every key and field behave that way for plugins; the loader's own lookups keep the version map.

Fields follow the same shape: the version map's row for the running CL first (else the latest CL's),
then the plugin's, then `builtOffset` on its `builtCL`. Two rows for one CL that disagree fail
the request. A reflected field is read live from reflection, and a row is only a cross-check:
where a row and the game disagree the game wins, and the disagreement is logged. It is a warning
when the row is the running build's own (a version map bug), and a debug line when the row is another
build's, which is expected to differ (the latest row stands in for a build with none). A native
offset (reflection does not describe it) comes from the running CL's row or the compiled offset
on the build it was compiled for; a row carried forward from an older CL cannot be checked on
this build, so the field is unresolved. `PLUGIN_FIELD_ANY_SIZE` (size 0) works for a native member
too: the running build's own row serves the offset and its size comes back, so a plugin that only
takes the member's address need not know the size; with no row for the running build it is refused.
A native offset plus its size must lie inside the
class's live size. See the SDK guide for the `[Map]` log lines.

**Struct sizes.** A size request is a field request with the member null (`SR_SIZE_CHECK`, or the
same request with size 0 for the live size). Its row is a class-keyed field row: the key is the
class name alone (no `::`), offset 0, the struct's size, mask 0. CI writes one per size key a
plugin requests, on every build. It is an ordinary field line, so the feed format is unchanged.

- A reflected struct (a `USTRUCT` or `UCLASS` the game describes) is answered from the running
  game's own reflection data (`UStruct::Size`), on the client and on the dedicated server, so each
  game build gives its own size. The row is only a cross-check: where it disagrees with the game,
  the game wins and the difference is logged, as a warning when the row is the running build's own
  (a version map bug) and as a debug line when it is another build's (the latest row stands in for a build
  with none).
- A struct the game does not reflect (a plain C++ type) has no live size. It is served from the
  running build's own row. With no row for the running build the request is refused; a row of
  another build is never used, since nothing on this build can confirm the size is still that.
  `PLUGIN_FIELD_NATIVE` on a size request changes nothing: the choice follows what the game
  reflects.
- A plugin asks with `PLUGIN_FIELD_CHECK` when its compiled code depends on the size (a struct
  copied by value, a `ProcessEvent` parameter block, a buffer a native function fills, a `TMap`
  value), and with size 0 and no flag when it only steps through an array, so it takes the size the
  game has.
- The compatibility report (`ModLoader\compat-report-<CL>.json`) lists every size a plugin
  requested under `sizes`, with the row's size next to the reflected one and a result (`match`,
  `mismatch`, `row-only`, `no-row`, `reflected-only`). It is written on a boot where a plugin asked
  for a size, whether or not the layout check found a change.

**Function parameters.** A UFunction is a reflected struct whose members are its parameters, so a
plugin can read where the running game puts them instead of trusting the compiled Dumper-7 parameter
struct. The class of the request is that struct's name, `<Class>_<Function>` with the game's class
name and no prefix letter (`CrEnviroWaveSubsystem_GetCurrentStageSettings`), or
`/Script/<Module>.<Class>_<Function>`. The Game-SDK's `SR_PARAM(Class, Function, Param)` is a
member request (`PLUGIN_FIELD_ANY_SIZE`, `Param` is `ReturnValue` for the return) and
`SR_PARAMS_SIZE(Class, Function)` is the size request. Both are reflected requests, answered live:
the member gives the parameter's offset in the parameter block and its size, and the size request
gives the function's `ParmsSize`, the end of its last parameter as the engine computes it (not
rounded up to an alignment). Locals of a Blueprint function are not parameters and are not
counted. Rows play no part: there is nothing to cross-check against, and no row is needed. A
name that is a class or struct is answered as one first. The name is split at every underscore
into class and function (functions have underscores too), and every native class of that name
that has that function answers; a name that matches no function, or more than one (the same class
name in two modules), is refused with the reason, and `/Script/<Module>.` picks one. A parameter
Dumper-7 renamed with a `_<n>` suffix (`Class_0`) is found under the name without it. Reads are
on the game thread, like the rest of the reflection data.

The owner may be a Blueprint generated class (the name ends in `_C`, `WBP_InventorySlot_C_SetSlotSize`),
which lives in a `/Game/` package, not in `/Script/`. Such a class is loaded by the game with the
screen that uses it and freed again, so it is normally not loaded when plugins load. The
Game-SDK's `SR_PARAM` and `SR_PARAMS_SIZE` carry `PLUGIN_FIELD_LATE` for it: a request whose class
is not loaded is not a failure but **pending**. The loader logs `pending` once (Info, no warning),
the compat report lists the key under "Waiting for a game class to load", and the feature that
lists the key is off and greyed in its config with a waiting reason; the plugin still loads, and no
failure popup opens. A key no declared feature lists cannot wait and refuses the plugin like any
miss. The plugin repeats the request from the game thread after `OnPluginLoadHooks` (the one
scanner call allowed there, `PluginMap::ResolveLateFields`) until it resolves. The loader walks
the game's objects at most once every two seconds per key while pending, only on the game thread,
and never again once resolved: it copies the parameter names, offsets and sizes out of the class it
sees and keeps no object pointer, so the numbers hold for the process even after the class is
freed and reloaded. Only `CPF_Parm` properties count, so the locals of a Blueprint function neither
appear as parameters nor stretch the block size. A class that is loaded and has no such function or
parameter, or two that differ, is refused once with the usual reason. Native keys are unchanged.
Flag only: the interface stays v75; an older loader ignores the flag and refuses a Blueprint class,
so the feature stays off there.

**Unreflected native types.** A plain C++ engine type that is neither a `UCLASS` nor a `USTRUCT`
(`FMassEntityManager`, `FStructSharedMemory`) has no reflected size to bound a native member by.
When reflection reports that no class or struct of that name exists, the member is served from
the running CL's row alone, with no class bound, and the log line says so (`unreflected type, no
class bound`). A row for the running CL is required: the built offset is not used for such a type,
and a request on a build without a row is refused. If reflection is not ready, or finds the name
ambiguous, the request fails as before.

The game names its types without the prefix letter, so a plain type can share its name with a class:
`FTexture`, the render resource behind every texture, beside the class `UTexture`. An `F` name that
only a class carries is a plain type. An `F` name that a struct carries in another module, or a `U`
or `A` name that a struct or a class in another module carries, is a request that missed, and fails.

## Unknown builds

A client on a game build the version map has no rows for does not stop. The gate lets it through
and every key is checked on this exe, so a plugin gets a value only when the game confirms it:

1. A function key: the newest build's row is rescanned here, as in [Selection](#selection). It
   counts only if its pattern resolves uniquely and as the kind asked, and your own pattern must
   agree with it. The address recorded for another build is never used.
2. A reflected member, its size, a struct size and a function's parameter size come from the game.
   A row of another build is only a cross-check.
3. A native member, a struct size the game does not describe, and an `SR_SIZE_CHECK` on a native
   member have nothing to check against, so they wait for the build's rows: the request fails.
4. A request that fails turns off the declared feature that lists its key, or refuses the plugin
   if none does. Admission is unchanged.

So put the keys you can do without into a declared feature. Example: a plugin's "Icons" feature
lists a native member key. On an unknown build the member's request fails, "Icons" is off, and the
rest of the plugin runs.

The loader writes `[UnknownBuild] Session` in `modloader.ini` before it hooks anything and clears
it on a clean exit. A session that ends any other way keeps mods off at the next start on that
build until `Retry=1` is set. `Enabled=0` keeps the old refusal. The server always refuses.

## Native members the image library reads

The image library reads a game texture through eight native members. Each is a field row for the
running CL, keyed `Class::Member`, and the loader asks for it by name; none is compiled in.

| Key | Used for |
|---|---|
| `UTexture::PrivateResource` | The texture's render resource |
| `FTexture::TextureRHI` | The RHI texture behind it |
| `FD3D12Texture::ResourceLocation` | Where the D3D12 texture keeps its resource |
| `FD3D12ResourceLocation::UnderlyingResource` | The `FD3D12Resource` itself |
| `FD3D12Resource::DefaultResourceState` | The state the engine leaves the resource in |
| `FD3D12Resource::Resource` | The `ID3D12Resource` to copy from |
| `FD3D12Resource::Heap` | The resource's heap, null for a committed resource |
| `FD3D12Heap::Heap` | The `ID3D12Heap` |

The first six are the chain from a texture to its resource. The last two are the heap pair that checks
the memory is resident while a texture is copied. Rows exist for the game builds the loader lists in
`hooks/game/version_map.generated.h`.

A build without the whole chain gets no image from the image library; mods show a placeholder. A build
without the two heap rows makes the resource resident instead of its heap while the copy runs.

**The older icon copy.** The loader's older icon copy (`LoadFromUTexture2D`) reads the same six chain
rows. On a build that does not have all six, it returns null, the same as the image library does.

## Features

A plugin can group the keys one part of it needs into a feature, with a plain-English title and
the config entries it drives. How to declare one is in the SDK guide
([Features](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md#features----declarefeature)).

What the loader does with it:
- A key that fails (step 4 of [Selection](#selection)) and belongs to a declared feature turns
  that feature off. The plugin still loads, `refused` stays false, and `GetCommitSerial()` still
  bumps so the failure window opens.
- A key that fails and belongs to no feature refuses the plugin, as before.
- A field key the version map lists as absent on the running build is not a failure. The feature
  goes off and the plugin loads, but it is not in the failure window, `GetCommitSerial()` does
  not bump, and the log line is
  `[Map] <plugin>: feature "<title>" is not in this game version: <keys>`. The hook failures
  report lists it under "Not in this game version", and the config page says
  `<title> is not in this game version`. With no feature listing the key, the plugin is refused
  as for any other key.
- The failure window lists the feature by its title; the log has one line,
  `[Map] <plugin>: feature "<title>" is off: <keys> (<reason>)`.
- Its config entries are reported as off (`PluginHookReport::ConfigOffReason`) so the config page
  can grey them out. Stored values are kept.

A plugin is still refused, whatever it declared, for an interface newer than the loader's, for no
built CL, and for missing core globals (`GetSdkGlobals`); see [Admission](#admission). The Plugins
tab shows a plugin with features off as `Loaded (N off)`; the README has the
tab's states.

## Admission

Each plugin is judged against the build it was compiled for, not the loader's. `PluginInfo` ends
in a tail from interface v72: `gameBuiltCL` (the build its Game-SDK came from,
`PLUGIN_GAME_BUILT_CL`) and `mapFlags`.

- On its own built CL a plugin loads as it always did.
- On any other supported CL it is admitted only if it is interface 72 or later, sets
  `PLUGIN_MAP_MIGRATED` in `mapFlags`, and called `GetSdkGlobals` in `PluginInit` (the call is what
  counts; a build where the loader cannot find GObjects, `FName::AppendString` or `GWorld` hands
  the plugin none, with a log line saying which). Otherwise the Plugins tab shows it as **Incompatible**, with "Needs an update to
  run on CL-...", and it still loads on its own build.
- On its own built CL it is also not refused when the startup layout check finds changed classes, as long
  as it declares that CL (interface 72 or later, `gameBuiltCL` set) and it is not the build the loader
  itself was built against. Those classes come from the loader's
  own SDK, and the plugin's SDK is the running build's. The loader takes the declared CL on the plugin's
  word and does not check its headers. A plugin that declares no build, or another one, is still
  refused then unless it sets `PLUGIN_MAP_MIGRATED`.
- A v70 or v71 plugin has no tail and counts as built for `kLegacyPluginCL` (127004), the build
  the last loader without a version map required. So does a v72 or later plugin whose `gameBuiltCL` is 0,
  with the warning "declares no game build". A tail that sets `PLUGIN_MAP_MIGRATED` and names no
  build is refused on every build.
- An older interface is never refused for its age: the tables only grow, so a plugin gets the
  slots it was built with. Only an interface newer than the loader's is refused, as needing a
  newer mod loader. A plugin before v70 (StarRuptureTimer 1.3.0 is v46) counts as built for a
  build before CL-127004, which no supported build is, so it shows "Needs an update to run on
  CL-...: ... where a build before CL-127004 had it" and is never called. That also keeps a plugin
  from before the v62 break away from tables it cannot read.

`PLUGIN_MAP_MIGRATED` is the plugin's promise that it reads no game data from compiled-in
offsets or addresses (the checklist is in the SDK guide). The loader does not check that
promise; starrupture-ci checks it for the plugins it tracks.

## The dedicated server's table

The Server build carries a built-in version map of its own, `hooks/game/version_map.server.h`, kept by hand
because the generated header is the client's and is rewritten on every game update. It holds the rows for
one exe, the server exe CI measured (CL-127004, PE time stamp `0x2E3595FF`, image size `0x0E2A8000`):

- **Function rows** are addresses with no pattern: the start of one function, or the address of a game
  global (`GNames (NamePoolData)` and `GWorld`, which a plugin can ask for, and the loader's own
  `ModLoader/GObjects` key, which only the loader's own request reads). `FName::AppendString` is the
  `FString&` overload the Game-SDK calls: the game files hold three overloads of that name, so its row is
  the start the loader's own pattern for it matches, the same as the SDK's `Offsets::AppendString`.
- **Field rows** give a member's offset and size, and for a bool its mask (`0xFF`; a bit field has its bit).
  A **size row** is a class key (no `::`) at offset 0 that gives the struct's size. Both were measured from
  the server's own game files, and a member's size is the size a plugin reads it at, as in the client's
  table (`FTransform::Translation` is a `0x20`-byte vector register in the game files and is read as a
  `0x18`-byte `FVector`, so its row says `0x18`). The exceptions are three rows for plain C++ types, taken
  from the engine's type layout: `FObjectKey::ObjectIndex`, `FObjectKey::ObjectSerialNumber` and
  `TClassMap<EClassRepNodeMapping>::Map` (the header says what each rests on). The client's
  table has the same three rows, with the same values, on each build it lists.

The server serves a row of this table only on that exe: the CL, the time stamp and the image size must
all match. On any other exe the table is off, and unless a
[downloaded version object map](#the-dedicated-servers-downloaded-version-map) names that exe, a plugin
runs on its own patterns and rows, as it did before the table. The same applies to the lookups of absent
members and of field and size rows, and to the class bound on native members: the server bounds only a
member it has a row for on the running exe. The client's table and its lookups are unchanged. A plugin
gets a function row from the server's version map only when it asks for the map key
(`PLUGIN_SCAN_FLAG_MAP_KEY`) on the server build too, and a field or size row through the same member and
size requests it makes on the client.

**Game-SDK globals on the server.** The server resolves the same four Game-SDK globals as the client
(GObjects, GNames, `FName::AppendString` and GWorld) through the same registry rows, and hands them to a
plugin through `GetSdkGlobals` when the version map serves the running server build (the same match on CL,
time stamp and image size as above), GObjects has passed the loader's structural check after engine init,
and `FName::AppendString` was found (GWorld too, off the build the loader is compiled for). On any other server
exe the loader resolves none of them, the SDK keeps its compiled offsets and `GetSdkGlobals` returns none, as
before. With no downloaded version map the server lists and runs on CL-127004 only.

## The dedicated server's downloaded version map

A Server build can add rows and game builds from a signed version object map (version map) of its own.
The file has the same format, the same Ed25519 signatures and the same trusted keys as the client's.

- **What is downloaded.** `version-map-server.v1.txt`: one `build` line per server game build, each with a
  source starting `server ` (for example `server steam_build 25642172`), then that build's function,
  field and size rows. A server loader refuses a file with no build line, or with a build line that does
  not start with `server `, and a client loader refuses a file that has one. The signature is checked first.
- **Where it comes from.** The compiled define `VMAP_SERVER_FEED_URL` (`/p:VmapServerFeedUrl=` in
  `Shared.props`), like the client's, never from `modloader.ini`. A Server build made without one fetches
  nothing and logs `no feed URL is compiled in`. The check, the limits (https only, 2 s a step, 8 s
  in all, 1 MiB) and the ETag are the client's, as described in [The feed](#the-feed). It is asked for with the
  game's main thread parked at startup; when that is not so, or the build is refused, the file is asked
  for for the next launch.
- **How long the server waits.** At most 8 s for the request, whatever the network does: redirects
  and connection retries are inside that limit. The request runs on a thread of its own. When it has not
  ended in time, the server logs that it is given up on and carries on with the cache. What it answers
  later is dropped: it is not stored and changes nothing, and the next start asks again. The 8 s is for
  the request only; reading the cache from disk is outside it.
- **Where it is kept.** `ModLoader\version-map-server.v1.cache`, with its ETag in
  `version-map-server.v1.cache.etag`: the client's cache has a different name, so neither reads the other's,
  and a file of the other kind is refused before it is stored.
- **How an exe is matched.** Every set of rows belongs to one exe: its game build number, the exe's time
  stamp and its image size. The file's rows for a build are used only when its `build` line equals the
  running exe in all three. A line with the same build number and another stamp or size gives the running
  exe nothing, new rows included. Unlike the client, the server never carries a row from one build to another.
- **On the exe the built-in table was measured on**, a matching line only adds rows, for names the table
  has no row for. A name the table has, as a function, a field or a size, under any fingerprint, or as
  absent, is served from the table alone: the file cannot change it, add a second row beside it or fill an
  absence. Each added row is logged once at start (`[VersionFeed]   added from the file: <key>`), and a
  file row for a table name that says something else is counted as a clash in the merge line.
- **On any other exe that a line matches**, for example a rebuilt exe of build 127004, the file's rows
  replace the table: nothing of the table is served there. The file therefore has to hold every row that
  exe needs. For the loader's own patterns and its plugins' keys, a row the file lacks leaves them on
  their own patterns and rows.
- **When rows are missing.** On an exe that neither the table nor the file names, no row is served and each plugin
  runs on its own patterns and rows. The version check goes by build number alone: a build number that
  neither has is refused. Where the running exe has rows but not one a plugin needs, the plugin gets
  its own pattern or row if it has one for that exe, and otherwise the loader refuses that request and logs
  it. For a native member with no row, the plugin's compiled offset is used only on a build number it was
  built for. The fix is a newer file with the missing rows (or a newer loader); it is read at the next
  start.
- **Turning it off.** `[AutoUpdate] Enabled=0` in `modloader.ini` makes no request at start, so the start
  is not delayed. A file already cached is still read and applied; delete
  `ModLoader\version-map-server.v1.cache` to run on the built-in table alone.
- **What the server checks.** The coverage rule the client uses (a game build from a file is kept only if
  it has a row for each loader entry and field) is empty on the server: the loader finds its own entries by
  their own patterns and reads its own fields from the game, so the file needs no `ModLoader/` rows. On a
  server build number other than the one the loader was compiled for, the loader reads its fields from
  the game and uses a file's row only to cross-check (a rebuilt exe of the compiled build number keeps the
  loader's compiled field offsets); if a field or a game global cannot be found it logs
  which and starts the game with no plugins. A plugin that takes its game data from the version map
  (`PLUGIN_MAP_MIGRATED`) runs on the second build with its fields read per build, from that build's rows.
  A plugin that does not stays refused there.
- **How it is published.** `release.yml` and `draft.yml` build the Server configuration with the address of
  the file on the loader's Latest release (`/p:VmapServerFeedUrl=`, the Client build's `/p:VmapFeedUrl=`
  twin). `vmap-publish.yml` copies the signed server file there from the `vmap-feed` release, a leg of its
  own beside the client's, with its own `seq`, and refuses a file of the wrong kind. The server leg does
  nothing until the `vmap-feed` release holds a server file, and the first one needs a manual run with
  `first_publish` set, as the client's did.
- **The layout check.** The server runs the same start-up layout check as the client, against its own table
  (`core/layout_table_server_127004.generated.h`, made from the Game-SDK's Server folder with
  `tools\Update-LayoutTable.ps1 -Server`). It lists the classes the server build reads, not the client's UI
  and input ones. A changed class that the loader's own code, or a layout plugins keep compiled in, reads
  switches the plugins off for the session, with the changed classes in `ModLoader.log` (`[LayoutCheck]`
  lines, then `plugins are switched off for this session`). A class only a version map key names does not: a
  plugin that keeps that layout compiled in is refused, one that takes it from the version map is not, as on
  the client. On a server game build other than CL-127004, a check that could not run adds a line to the list
  that keeps the server's plugins off; on CL-127004 it warns and the server carries on. The compatibility
  report and the menu-state switch-off stay client-only. Every difference is logged as its own
  `[LayoutCheck]` line, so a false alarm on CL-127004 can be read from `ModLoader.log`. To exempt a member that
  only plugins read, at an offset that moves between builds, add it to `kPluginPerBuildMembers` in
  `core/layout_check.h` (it needs a version map field row on every build). A member the engine keeps outside
  reflection (`ULevel::Actors`) has no entry in the game's reflection data, so it is listed in
  `kUnreflectedMembers` in the same file: it is not compared when the game lacks it, and the summary line
  counts it under `rows the game does not reflect`. The table generator already leaves such a member out,
  so no table has that row now and the count is 0; the list is a net for one the generator misses.
- **What the log says.** After the merge line, `[VersionFeed]` prints one line per build in the file:
  `this exe` or `other exe` (with its stamp and size) and its row count. For build 127004, which the
  loader's table has, it then says how many rows were added to the table (`the rest not used`), that the
  file's rows are `used in place of the loader's own`, or that they are `not used on this exe`. For any
  other build it says the build was added to the version map. Then comes one `added from the file:` line
  for each row added to the table.

To try it without a published file, build the Server configuration with `VMAP_TRUST_TEST_KEY` (the
test key, `k1`), sign a server file with that key, and copy it byte for byte to
`ModLoader\version-map-server.v1.cache` next to the server exe. With no address compiled in it fetches
nothing and reads the cache. `version_map_server_tests` does this with an in-process key.

## The feed

Between loader releases the version map can be refreshed from a signed file, so a new game build is
supported without a new loader.

**Format.** A tab-separated text file, one row per line, in the order `format`, `seq`, `build`,
`function` / `absent`, `field`, then a last line `sig<TAB><keyId><TAB><base64>`. `format` is `1`;
the loader reads only the format it was built for. `seq` is the UTC second of signing. `sig` is an
Ed25519 signature (RFC 8032) over every byte before that line. The row layout is in
starrupture-ci's `ci/VERSION-MAP.md`.

**Asset.** `version-map-client.v1.txt`, on the loader's Latest release. The name carries the
format version, so a later format never strands a loader that reads this one. Latest is where it
sits because `prune_releases.yml` deletes the others. `vmap-publish.yml` puts the current signed
file on Latest: a new release dispatches it itself (the run every 15 minutes is the fallback), and
between releases whenever the `seq` rises. The first release that carries one needs a manual run with `first_publish` set.
The dedicated server has a file of its own, `version-map-server.v1.txt`, in the same format, which the
same workflow puts on Latest in a leg of its own (its own `seq`, its own `first_publish`); see
[The dedicated server's downloaded version map](#the-dedicated-servers-downloaded-version-map).

**Fetch.** The URL is the compiled define `VMAP_FEED_URL` (`/p:VmapFeedUrl=` in `Shared.props`),
never read from `modloader.ini`. An empty define, or no compiled public key, means no fetch. The
client build fetches this file and the server build its own (below), with `[AutoUpdate] Enabled` on.
The one rule: at every start it asks whether a newer feed than the cached one is out, and if so
applies it on that start.

- The question is asked at startup, with the game's main thread parked (where WinHTTP is safe), before
  the version check, so a newer feed is read by that very start. When the running exe is in neither the
  built-in version map nor the cache, the splash shows "Downloading game data for CL-n" while it waits;
- after the version check or the pattern preflight refuses the game build, it is asked once more for the
  next launch, before the process exits, if the startup request did not already ask;
- the auto-update thread asks for the next launch only if nothing asked at startup this launch.

`ModLoader.log` says what the check found: a newer version object map was stored (and then "Newer rows for
the version object map were fetched and are read on this start"), none is newer, or it could not be
fetched, was refused, or was not stored, in which case the cache is as it was.

**Request.** One HTTPS GET, conditional. The loader keeps the `ETag` of the answer the cache came from
next to the cache (`version-map-client.v1.cache.etag`, `<seq> <etag>`) and sends it as `If-None-Match`,
so a feed that has not changed comes back as a 304 with no body. The ETag is sent only when its `seq`
is the cache's; with none on file, or a server that sends none, the whole feed (about 330 KB) is asked
for and compared by `seq`. The feed is a release asset on `github.com`, not the GitHub REST API, so
there is no rate limit to share. A URL that is not `https` is refused, redirects from HTTPS to HTTP are
disallowed, each operation has a 2 s timeout, the whole request has an 8 s deadline, and the body
is capped at 1 MiB while it is read (`feed_fetch.h`, `http_get.cpp`). These cut the request off: on a
network that drops the connection, a host that does not answer costs about 4 s and an unroutable one
about 6 s (measured), and then the game starts on the cache. The deadline is checked between steps,
and redirects and connection retries run inside one step, each operation with its own timeout, so a
request that is slow at every one of them can take longer than 8 s. The dedicated server also bounds
its wait, so there
8 s is the most ([above](#the-dedicated-servers-downloaded-version-map)).

**Cache.** The signed file itself, `ModLoader\version-map-client.v1.cache`, written to a temp
file and then renamed over. It is verified again on every launch and only ever replaced by a file
with a higher `seq`, so an old signed file cannot be replayed over a newer one. No network, a 404, a
timeout or a bad signature leave it as it was, and it keeps working.

**Signature.** The file is verified before anything in it is read. It is rejected whole if it is
over 1 MiB, has a malformed row of a kind it knows, has a `sig` line that is not last, names a key
the loader does not hold, or has a bad signature. Rows of a kind the loader does not know are
skipped, and so are columns past the ones a known kind has, so a newer feed can add both. Verification uses vendored Monocypher (Ed25519 with SHA-512),
which has no handles and behaves the same on Windows and Wine.

**Keys.** Two public-key slots are compiled in (`version_feed.cpp`), so a key can be rotated by a
loader release that adds the new key and a later one that drops the old.

| Slot | Key | Trusted by |
|---|---|---|
| `k1` | TEST key | only a build that defines `VMAP_TRUST_TEST_KEY` (the unit tests and `sr-aobscan`) |
| `k2` | release key | every build |

A release build therefore trusts only `k2`. A feed signed with `k1` is rejected by it. The
private halves are CI secrets (`VMAP_SIGN_KEY_TEST`, `VMAP_SIGN_KEY`); a leaked key is answered by
a loader release that drops its slot.

**Merge.** This is the client's rule; the server's is
[above](#the-dedicated-servers-downloaded-version-map). The loader merges the built-in version map
and the feed into one row per key and CL (per `fp` for a site key), once at startup, and the result
stays unchanged for the process. For each group it takes the built-in row for the running exe if
there is one, otherwise the feed's, otherwise the built-in. The feed can add builds and keys and
fill a shipped `absent` row; it can never change a locator the loader shipped with for the running
exe. A CL known only from the feed is supported only if the union holds a locator for every
required loader entry and loader field on it; otherwise it is dropped and the build stays unknown.

**What a bad feed can do.** It cannot run code. Every row still has to pass its kind check, the
uniqueness rule, equality with its `rva` on its own exe, and for a native field the class-size
bound. The worst a wrong signed row can do is point a plugin's hook at the wrong game function,
which is why the file is signed.

## How a build reaches players (CI)

[starrupture-ci](https://github.com/msantoro12/starrupture-ci) watches Steam and does the work.
"Path M" is the update that is only rows in the version map. The tools are described in its
`ci/MAP-MODE.md`.

```
new Steam build
  -> client check       build, scan patterns, PDB layouts
  -> map-mode.mjs       function and site rows from the harness (sr-aobscan)
  -> field-rows.mjs     field rows from the layouts
  -> decide.mjs         path M / B / C, from the gate results
  -> export-feed        the header as the unsigned feed
  -> vmap-sign.mjs      sign with the test key (k1) and verify
  -> staging            a plan for the loader fork's auto/update-<CL> branch
  -> promote.mjs        a plan: only when public is the same build as the staged one
  -> publish            sign with k2, the vmap-feed release in starrupture-ci (not yet wired, below)
  -> vmap-publish.yml   in the loader repo: verify, upload to Latest
  -> players            fetch on their next launch
```

1. **Client check.** Checks the build and scans the loader's and the plugins' patterns on it.
2. **Rows.** `map-mode.mjs` writes a function row per key (the pattern the harness saw match at
   the key's address, otherwise a high-confidence suggestion unique on the build, otherwise an
   `absent` row) and `field-rows.mjs` writes the field rows.
3. **Decide.** `decide.mjs` turns the gates into a path. M is a clean update: rows only. B is a
   required pattern missed, repaired by a source patch. C is a used class that moved, held until
   that path is proven. A used class that moved is settled under M when the field rows cover it
   with a row, an absent row or a class-size move. It reads the client check's gates, not the
   plugin scan results.
4. **Export, sign, verify.** The header is exported as the feed and signed with the test key.
   `seq` is the larger of the clock and the last published `seq` plus one, and the signer refuses
   a `seq` more than a day ahead.
5. **Stage and promote.** Staging and promotion are plans: a beta build is staged, and promoted
   when public flips to the same build (same build id, exe SHA-256 and CL). The gate that re-scans
   the DLLs on the public exe is not yet backed by a real re-scan.
6. **Publish.** Not yet wired: the staged index promote reads is not stored anywhere, so a public
   run always plans the normal flow and the `publish` job never runs. Once it does, the job signs
   with `k2`, verifies the result without the test key, and replaces the asset on the `vmap-feed`
   release. The release key has to be held somewhere only the publishing job on `main` can read
   before that happens. `vmap-publish.yml` in this repository pulls the asset every 15 minutes,
   checks it against the keys in `version_feed.cpp`, and uploads it to Latest only if it verifies
   and its `seq` is higher than the newest one on a release (or, on a Latest that has none yet, no
   lower). This repository holds none of the signing secrets.

**Publishing is off by default.** The `publish` input of `sr-client-check.yml` defaults to false,
and `state/autopilot.json` decides what may run unattended: its `enabled` switch, the `paths` it
allows (`M`) and `max_releases_per_day`. With `enabled: false`, `decide.mjs` returns HOLD
("autopilot is disabled") on a clean update, and nothing is exported, signed or published until
the owner turns it on. `vmap-watchdog.yml` opens an issue when Latest's feed lags the last signed
`seq` by more than two hours.

## Getting a value added to the shared version map

A plugin does not have to wait for the shared version map. It can carry its own row for a key (the SDK's
`plugin_version_map.h` builds them into the plugin), and the loader uses the shared row first
and the plugin's own row second (see [Selection](#selection)). A plugin written for interface v73
or later can also declare features, so a value that does not resolve turns one feature off instead
of refusing the plugin. Older plugins are accepted too, as [Admission](#admission) says.

A plugin's own rows are only as current as its last release. A released plugin reads every game value from
the shared version map and loads on every current game build, so its rows move there before its release.
To have the pipeline keep a value current on every game build, open an issue on the
[SDK repository](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/issues) with your plugin's name and the
`Class::Member` list. We add the key to the shared version map and to the CI key list, and sign and publish
the rows. What we add:

1. **A function or global.** We add the plugin's rows for the build you tested to
   `hooks/game/version_map.generated.h` (`kFunctionRows`), with the `rva` you verified. The key is
   `Class::Function` for a game symbol. A site key is `<PluginName>/<hookName>` and carries the
   request's `fp`. The CI pipeline regenerates these rows for each new build, from the scans of
   the plugins it tracks (the `plugins` input of `sr-client-check.yml`), so we add your plugin
   there too.
2. **A member.** We add a row to `kFieldRows` for the build you tested, and a line to
   `ci/native-keys.txt` in starrupture-ci so `field-rows.mjs` reads it from the PDB on every build.
3. **Optional or required.** Tell us which. A key is optional if the plugin runs without it. Mark it `optional`
   on the `native-keys.txt` line so a miss on a new build is not a failure; a key is optional
   only when every plugin's line for it says so.

The row formats, from the generated header:

    // MiniMap/FBulkData::GetBulkDataSize
    { 127004, 0xE8972298, 0x0F35C000, 3, 0x26FFCBE0, false, "44 0F B6 49 0C ...", 0, -1, -1, 0x015BFD50, nullptr },
    // UCrPlayerMapMenuDataComponent::FOWData
    { 127004, 0x100, 0x10, 0x00, nullptr },

A function row is: CL, image stamp, image size, kind, `fp`, absent, pattern, `resultOffset`,
`followRel32At`, `followRipDispAt`, `rva`. A build whose exe you have no PDB for gets a pattern row:
the same row with `rva` 0, so the loader finds the function by scanning the pattern (it must match
the running exe exactly once, as the kind the request asks for), with no address to check it against.
A field row is: CL, offset, size, bool mask. And the
`native-keys.txt` line for a member:

    MiniMap UTexture::SRGB optional

That is `<plugin> <Class::Member>`, then optionally an offset, size and mask in hex when the
PDB's size is not the plugin's, then `optional`. The header of `ci/native-keys.txt` has the full
format.

Before you ask, check that the plugin meets what the version map relies on. `PluginGameBuild.props`
(in the SDK) defines the plugin's `PLUGIN_GAME_BUILT_CL`, `PLUGIN_MAP_MIGRATED` is the promise
that it reads no game data from compiled-in offsets, and the SDK's rule-3 check
(`tools/rule3-check.mjs`) is the scan that backs that promise. Run `Tests\VersionMapTests` and
`node --test ci/*.test.mjs` in starrupture-ci; both must pass.

### Listing your plugin in the Browse tab

The Browse tab in the overlay shows the plugins in `mod-index/index.json`. You add your plugin the same
way: open a pull request that adds one object to the array. The fields and a worked
example are in [mod-index/README.md](mod-index/README.md). `id`, `name` and `page_url` are required;
please add `readme_url` (a raw GitHub link to your README) and `tested_loader` (the ModLoader version you
tested on) too.

Save the file with LF line endings and a final newline, and check it before you send it:

    node .github/vmap/build-mod-index.mjs mod-index/index.json --out mod-index.txt

It says `ok`, or names the entry that has to change.

After your PR is merged, the maintainers sign and publish the list. A ModLoader release is
not needed.

## Failure modes

What a player sees, and what the log says (`ModLoader\Logs\ModLoader.log`).

| Case | Result |
|---|---|
| The running build is in the version map | Starts, after the one small check for a newer feed that every start makes (a 304 when there is none). With no network the check fails quietly and the built-in version map and the cache are used. This is the normal case. |
| New build, no feed published yet, or no network | The client runs on checked values from the newest build the version map lists (see [Unknown builds](#unknown-builds)), and the overlay says so. With `[UnknownBuild] Enabled=0`, the gate refuses the build: the player gets the "Start Game Without ModLoader" dialog and can play unmodded or quit. A server always refuses. The loader tries again for the next launch. |
| A session on an unknown build did not end cleanly (crash, hang, forced close) | The next start on that build hooks nothing, and a dialog says how to try again: `Retry=1` under `[UnknownBuild]` in `modloader.ini`. A game patch or another loader build tries again by itself. |
| New build, feed published | The splash shows "Downloading game data for CL-n" for a moment, then the game starts modded. |
| A fetch fails (404, timeout, over 1 MiB, not https) | As if there were no feed: a known build runs, an unknown one runs on checked values. The loader asks again at the next launch, as it does at every launch. |
| The feed's signature is bad, the key unknown or the file malformed | The file is discarded whole, and the cache is untouched. Same as no feed. |
| The feed is older than the cache (`seq` not higher) | Ignored. The cache stays. |
| A release build is handed a `k1` (TEST) feed | Rejected as a bad key. |
| `[AutoUpdate] Enabled` off, or a server build with no feed address | No fetch. The built-in version map is used. A server stays exact on the build it was written for, and its built-in version map is its own table (see [The dedicated server's table](#the-dedicated-servers-table)). |
| A server finds a client file, or a client a server file | The file is refused whole, the log says "not a dedicated server file" or "a dedicated server file, which a client loader does not read", and the cache is untouched. |
| The feed has the build, but a row is `absent` for something a plugin needs | That plugin's request fails. If it belongs to a declared feature, that feature is off; otherwise the plugin is refused, with the reason in the failure window. Other plugins and the game run. |
| The feed has the build but misses a required loader entry | That CL is dropped from the feed, and the build is unknown as above. |
| A plugin built for another build does not declare `PLUGIN_MAP_MIGRATED` | **Incompatible**: "Needs an update to run on CL-...". It loads on its own build. |
| A plugin's own pattern matches 0 or 2 or more places, but a row resolves the key | The plugin loads on the row. `ModLoader.log` has one `[Map]` warning naming the plugin, the key and the count. Where no row resolves the key it would be refused, so the author should make the pattern unique (or update it, for 0). |
| Two keys for one request disagree (AMBIGUOUS) | The request fails, with the locators listed: a declared feature turns off, any other key refuses the plugin. |
| The game's `UObject` virtual table no longer holds `ProcessEvent` where the SDK calls it | Every plugin is switched off for the session, with the same dialog as a layout change. The log line is `[LayoutCheck] UObject::ProcessEvent: vtable slot 0x4C holds base+0x..., the function is at base+0x...`; when it is fine, the log says the slot holds the function the version map resolved. |
| A game class a plugin reads moved | That plugin's own field request, or its `SR_SIZE_CHECK`, fails and the plugin alone is affected (a declared feature off, else refused), with the reason in the failure window. The loader's layout check covers the classes the loader and the Plugin SDK read, and what a change switches off depends on who reads the class: see the README. After any change, a plugin that keeps the game's layouts compiled in is not loaded, unless it declares the build that is running (its layouts are that build's); one that declares `PLUGIN_MAP_MIGRATED` (taken on its word) still loads, and a request of its for the compiled offset of a changed class, or of a class derived from one, is refused, which turns off only the feature that asked, unless the request says its SDK came from the build that is running. |

A fetched version map is read once at startup and never changes mid-session, so a RELOAD sees the same
rows the loader's own hooks were installed from.
