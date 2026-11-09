# Version map

How the mod loader finds game functions and game-class members across game builds, how a new
build reaches players without a loader release, and what each failure looks like. For plugin
authors the request side is in the
[SDK guide](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md);
for players the short version is the README's [Game updates](README.md#game-updates).

A game update moves functions and fields. The version map is the one place that says where they
are on each build, so a new build means new rows in the map and nothing else: no loader release
and no plugin release, as long as the plugins take their game data from the map.

## What the map holds

The map has one row per key and game build (a build is its CL, plus the exe's PE time stamp and
image size). There are four kinds of row.

| Row | Key | Holds |
|---|---|---|
| function | `Class::Function` or a global's name (`GWorld`) | A pattern, `resultOffset`, `followRel32At` and `followRipDispAt` as in a `PluginScanRequest`, the `rva` CI verified, and the `kind` it was verified with |
| function (site) | `<owner>/<hookName>` | The same, plus `fp` |
| data | a global's name | A function row of kind `DATA` |
| field | `Class::Member` | `offset`, `size`, a bool mask, and the member's new name when it was renamed |
| absent | any function key | No locator: CI could not place the key on that build |
| absent (field) | `Class::Member` | CI only: a member CI could not find on that build. It is the `kAbsentFields` array in the generated header; the loader never reads it, and a feed has no such line |

**Key.** A key names a function, a global or a member in Dumper-7 naming. A global key (kinds
`FUNCTION_START` and `DATA`) names one game symbol, the same for every plugin, and has no `/`. A
site key (kinds `IN_FUNCTION`, `CODE`, `VTABLE`, `ANY`) names no symbol. It is
`<owner>/<hookName>`, where the owner is the plugin's `PluginInfo` name or `ModLoader` for the
loader's own. Only that owner reads it.

**Kind.** A row is used only for a request of the same kind. Otherwise it is skipped with a log
line, so the map can never weaken a kind check.

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

The map lives in `hooks/game/version_map.generated.h`, written by CI
(`ci/version-map.mjs` in [starrupture-ci](https://github.com/msantoro12/starrupture-ci), format in
its `ci/VERSION-MAP.md`). Every loader build carries the map as it stood when it was built, and
works offline on every build in it. The signed feed (below) adds to that.

**Not map data.** Some values a plugin needs are not in the map on purpose.

- *Struct sizes.* A size request is a field request with the member null (`SR_SIZE_CHECK`, or the
  same request with size 0 for the live size). Every game struct a plugin sizes is a reflected
  `USTRUCT`, so the loader answers from the running game's own reflection data (`UStruct::Size`),
  on the client and on the dedicated server, and each game build gives its own size. A row could
  only repeat what reflection says, so there is no size row. A class-keyed row (a key with no `::`)
  is read as a cross-check, like the row of a reflected member: where it disagrees with the game,
  the game wins and the difference is logged. A size request is never native: `PLUGIN_FIELD_NATIVE`
  on it is a bad request. A plugin asks with `PLUGIN_FIELD_CHECK` when its compiled code depends on
  the size (a struct copied by value, a `ProcessEvent` parameter block, a buffer a native function
  fills, a `TMap` value), and with size 0 and no flag when it only steps through an array, so it
  takes the size the game has. A new line type in the feed would also make every 1.23.x loader drop
  the whole signed feed, which is another reason not to add a size row.
- *Game enums.* The numbers in a game `UEnum` can move on a game update, and no symbol or pattern
  holds them, so CI could not generate a row. The value belongs to the game's reflection data, read
  by the enum's and the entry's name.
- *Unreal Engine constants.* `EObjectFlags` bits (`RF_*`), `FUNC_Native`, `INDEX_NONE` and the
  engine ABI the plugins compile in (the `TArray` header, shared-pointer reference controllers) are
  fixed by the engine version, not by a game build. A change of engine version regenerates the whole
  Game-SDK, so the map has nothing to add. The one the map can help check is the slot of
  `UObject::ProcessEvent` in the object's virtual table, which every plugin's SDK compiles in: at
  startup the layout check compares that slot with the address the map resolved for the function,
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
be refused wherever no row resolves the key (a loader with no map, a dedicated server, a build none
of the rows resolve on), since the pattern is then the only locator. A pattern that matches 0 times
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

A key that is not in the map at all (a third-party plugin, or a hook newer than the feed) falls
back to the plugin's own row and pattern, logged as `not in the loader's map; own pattern`.
To test a plugin's own locators, `DisablePluginVersionMap=1` under `[Debug]` in `modloader.ini` makes
every key and field behave that way for plugins; the loader's own lookups keep the map.

Fields follow the same shape: the map's row for the running CL first (else the latest CL's),
then the plugin's, then `builtOffset` on its `builtCL`. Two rows for one CL that disagree fail
the request. A reflected field is read live from reflection, and a row is only a cross-check:
where a row and the game disagree the game wins, and the disagreement is logged. A native
offset (reflection does not describe it) comes from the running CL's row or the compiled offset
on the build it was compiled for; a row carried forward from an older CL cannot be checked on
this build, so the field is unresolved. A native offset plus its size must lie inside the
class's live size. See the SDK guide for the `[Map]` log lines.

**Unreflected native types.** A plain C++ engine type that is neither a `UCLASS` nor a `USTRUCT`
(`FMassEntityManager`, `FStructSharedMemory`) has no reflected size to bound a native member by.
When reflection reports that no class or struct of that name exists, the member is served from
the running CL's row alone, with no class bound, and the log line says so (`unreflected type, no
class bound`). A row for the running CL is required: the built offset is not used for such a type,
and a request on a build without a row is refused. If reflection is not ready, or finds the name
ambiguous, the request fails as before.

## Features

A plugin can group the keys one part of it needs into a feature, with a plain-English title and
the config entries it drives. How to declare one is in the SDK guide
([Features](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md#features----declarefeature)).

What the loader does with it:
- A key that fails (step 4 of [Selection](#selection)) and belongs to a declared feature turns
  that feature off. The plugin still loads, `refused` stays false, and `GetCommitSerial()` still
  bumps so the failure window opens.
- A key that fails and belongs to no feature refuses the plugin, as before.
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
- A v70 or v71 plugin has no tail and counts as built for `kLegacyPluginCL` (127004), the build
  the last loader without a map required. So does a v72 or later plugin whose `gameBuiltCL` is 0,
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

## The feed

Between loader releases the map can be refreshed from a signed file, so a new game build is
supported without a new loader.

**Format.** A tab-separated text file, one row per line, in the order `format`, `seq`, `build`,
`function` / `absent`, `field`, then a last line `sig<TAB><keyId><TAB><base64>`. `format` is `1`;
the loader reads only the format it was built for. `seq` is the UTC second of signing. `sig` is an
Ed25519 signature (RFC 8032) over every byte before that line. The row layout is in
starrupture-ci's `ci/VERSION-MAP.md`.

**Asset.** `version-map-client.v1.txt`, on the loader's Latest release. The name carries the
format version, so a later format never strands a loader that reads this one. Latest is where it
sits because `prune_releases.yml` deletes the others. `vmap-publish.yml` puts the current signed
file on Latest: on a new release at its next run (every 15 minutes), and between releases whenever
the `seq` rises. The first release that carries one needs a manual run with `first_publish` set.
There is no server feed: only the client fetches.

**Fetch.** The URL is the compiled define `VMAP_FEED_URL` (`/p:VmapFeedUrl=` in `Shared.props`),
never read from `modloader.ini`. An empty define, or no compiled public key, means no fetch. The
loader fetches only on the client, with `[AutoUpdate] Enabled` on, and only:

- at startup, when the running exe is in neither the built-in map nor the cache, or has an
  `absent` row and the cache is over a day old. The splash shows "Downloading game data for
  CL-n". This happens with the game's main thread parked, where WinHTTP is safe;
- after the version check or the pattern preflight refuses the game build, for the next launch,
  before the process exits (not when the startup request just made one);
- from the auto-update thread, for the next launch: at most once a day while the cache holds a
  verified feed, otherwise at every launch.

An exe the map has never seen asks at every launch until a cached feed covers it; the once-a-day
limit applies to the `absent` case and to the auto-update thread. An empty, cut-short, oversize or
unreadable cache does not count as a verified feed.

**Request.** One HTTPS GET. A URL that is not `https` is refused, redirects from HTTPS to HTTP are
disallowed, each operation has a 2 s timeout, the whole request has an 8 s deadline, and the body
is capped at 1 MiB while it is read (`feed_fetch.h`, `http_get.cpp`).

**Cache.** The signed file itself, `ModLoader\version-map-client.v1.cache`, written to a temp
file and then renamed over. It is verified again on every launch and only ever replaced by a file
with a higher `seq`, so an old signed file cannot be replayed over a newer one.

**Signature.** The file is verified before anything in it is read. It is rejected whole if it is
over 1 MiB, has a malformed line, has a `sig` line that is not last, names a key the loader does
not hold, or has a bad signature. Verification uses vendored Monocypher (Ed25519 with SHA-512),
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

**Merge.** The loader merges the built-in map and the feed into one row per key and CL (per `fp`
for a site key), once at startup, and the result stays unchanged for the process. For each group
it takes the built-in row for the running exe if there is one, otherwise the feed's, otherwise
the built-in. The feed can add builds and keys and fill a shipped `absent` row; it can never
change a locator the loader shipped with for the running exe. A CL known only from the feed is
supported only if the union holds a locator for every required loader entry and loader field on
it; otherwise it is dropped and the build stays refused.

**What a bad feed can do.** It cannot run code. Every row still has to pass its kind check, the
uniqueness rule, equality with its `rva` on its own exe, and for a native field the class-size
bound. The worst a wrong signed row can do is point a plugin's hook at the wrong game function,
which is why the file is signed.

## How a build reaches players (CI)

[starrupture-ci](https://github.com/msantoro12/starrupture-ci) watches Steam and does the work.
"Path M" is the update that is only rows in the map. The tools are described in its
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

## Getting a value added to the shared map

A plugin does not have to wait for the shared map. It can carry its own row for a key (the SDK's
`plugin_version_map.h` builds them into the plugin), and the loader uses the shared row first
and the plugin's own row second (see [Selection](#selection)). A plugin written for interface v73
or later can also declare features, so a value that does not resolve turns one feature off instead
of refusing the plugin. Older plugins are accepted too, as [Admission](#admission) says.

A plugin's own rows are only as current as its last release. To have the pipeline keep a value
current on every game build, send a PR that adds it to the shared map and to the CI key list:

1. **A function or global.** Add the plugin's rows for the build you tested to
   `hooks/game/version_map.generated.h` (`kFunctionRows`), with the `rva` you verified. The key is
   `Class::Function` for a game symbol. A site key is `<PluginName>/<hookName>` and carries the
   request's `fp`. The CI pipeline regenerates these rows for each new build, from the scans of
   the plugins it tracks (the `plugins` input of `sr-client-check.yml`), so name your plugin there
   too.
2. **A member.** Add a row to `kFieldRows` for the build you tested, and a line to
   `ci/native-keys.txt` in starrupture-ci so `field-rows.mjs` reads it from the PDB on every build.
3. **Optional or required.** A key is optional if the plugin runs without it. Mark it `optional`
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

Before you send it, check that the plugin meets what the map relies on. `PluginGameBuild.props`
(in the SDK) defines the plugin's `PLUGIN_GAME_BUILT_CL`, `PLUGIN_MAP_MIGRATED` is the promise
that it reads no game data from compiled-in offsets, and the SDK's rule-3 check
(`tools/rule3-check.mjs`) is the scan that backs that promise. Run `Tests\VersionMapTests` and
`node --test ci/*.test.mjs` in starrupture-ci; both must pass.

## Failure modes

What a player sees, and what the log says (`ModLoader\Logs\ModLoader.log`).

| Case | Result |
|---|---|
| The running build is in the map | Starts with no network request. This is the normal case. |
| New build, no feed published yet, or no network | The gate refuses the build. The player gets the "Start Game Without ModLoader" dialog and can play unmodded or quit. The loader tries again for the next launch. |
| New build, feed published | The splash shows "Downloading game data for CL-n" for a moment, then the game starts modded. |
| A fetch fails (404, timeout, over 1 MiB, not https) | As if there were no feed: a known build runs, an unknown one is refused. An unknown build asks again at the next launch, and so does the auto-update thread while no verified feed is cached; the other triggers wait a day. |
| The feed's signature is bad, the key unknown or the file malformed | The file is discarded whole, and the cache is untouched. Same as no feed. |
| The feed is older than the cache (`seq` not higher) | Ignored. The cache stays. |
| A release build is handed a `k1` (TEST) feed | Rejected as a bad key. |
| `[AutoUpdate] Enabled` off, or a server build | No fetch. The built-in map is used. A server stays exact on the build it was written for. |
| The feed has the build, but a row is `absent` for something a plugin needs | That plugin's request fails. If it belongs to a declared feature, that feature is off; otherwise the plugin is refused, with the reason in the failure window. Other plugins and the game run. |
| The feed has the build but misses a required loader entry | That CL is dropped from the feed, and the build is refused as above. |
| A plugin built for another build does not declare `PLUGIN_MAP_MIGRATED` | **Incompatible**: "Needs an update to run on CL-...". It loads on its own build. |
| A plugin's own pattern matches 0 or 2 or more places, but a row resolves the key | The plugin loads on the row. `ModLoader.log` has one `[Map]` warning naming the plugin, the key and the count. Where no row resolves the key it would be refused, so the author should make the pattern unique (or update it, for 0). |
| Two keys for one request disagree (AMBIGUOUS) | The request fails, with the locators listed: a declared feature turns off, any other key refuses the plugin. |
| The game's `UObject` virtual table no longer holds `ProcessEvent` where the SDK calls it | Every plugin is switched off for the session, with the same dialog as a layout change. The log line is `[LayoutCheck] UObject::ProcessEvent: vtable slot 0x4C holds base+0x..., the function is at base+0x...`; when it is fine, the log says the slot holds the function the map resolved. |
| A game class a plugin reads moved | That plugin's own field request, or its `SR_SIZE_CHECK`, fails and the plugin alone is affected (a declared feature off, else refused), with the reason in the failure window. The loader's layout check covers the classes the loader and the Plugin SDK read, and what a change switches off depends on who reads the class: see the README. After any change, a plugin that keeps the game's layouts compiled in is not loaded; one that declares `PLUGIN_MAP_MIGRATED` (taken on its word) still loads, and a request of its for the compiled offset of a changed class, or of a class derived from one, is refused, which turns off only the feature that asked. |

A fetched map is read once at startup and never changes mid-session, so a RELOAD sees the same
rows the loader's own hooks were installed from.
