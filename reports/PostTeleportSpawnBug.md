# Falling Through The World On Load: The PostTeleport Timeout

A reverse-engineering writeup of why StarRupture players sometimes load into a save and
immediately fall through the terrain into the void, or drop off the machine they saved on and get
trapped underneath it when it spawns back in.

Short version: the game **has** a mechanism that holds you still until the world has streamed in
around you. It gives up after a hard 30 second wall-clock deadline and drops you wherever you
happen to be, world loaded or not. That deadline is a console variable:

```
Chimera.Movement.PostTeleportTimeoutDuration
```

Default `30.0`. Raising it to `180` turns "died in the void" into "loading screen stayed up a bit
longer". **But see [Setting the CVar](#setting-the-cvar) first -- in the shipping build this cannot
be set from `Engine.ini` or the command line, only in-process.**

---

## Symptoms

1. You load a save and fall through the terrain into the void, then die.
2. You saved standing on a platform / machine / habitat. On load you drop to the terrain, then the
   building spawns back in on top of you and you are stuck inside it.

Both are the same failure with a different branch taken. It correlates strongly with large bases
and slow storage, and on a given save it tends to be reproducible rather than random -- which is a
clue, and is explained below.

---

## What the game is supposed to do

The engine already knows this is dangerous and tries to protect against it.

```
UCrSaveSubsystem::OnPostLoadMap                 0x1475BAC10
  -> broadcast OnSaveLoaded
     -> ACrGameStateBase::OnSaveLoaded          0x14756B5C0
        -> per player controller:
           ACrCharacterPlayerBase::LoadPlayerCharacterData   0x14750EEF0
              1. restore the floor hint saved with the character
              2. TeleportTo(savedTransform, bIsATest=0, bNoCheck=1)
                 -> APawn::TeleportSucceeded
                    -> UCrCharacterMovementComponent::OnTeleported   0x1474C8EF0
                       -> UpdateTeleportState(bOnTeleported=true)    0x1474DD760
```

The "floor hint" is one of two things, depending on what you were standing on when you saved:

- ordinary geometry: `TeleportFinalFloorImpactPoint` + `TeleportFinalFloorName`
- a Mass-simulated building: `TeleportRequiredEntityPersistentIDToHaveActorVisualization` +
  `bWaitForMassEntityToUnlockMovementAfterTeleport`

`UpdateTeleportState` arms the hold: `TeleportWaitingForLevelStreaming = 4`,
`TeleportTimestamp = World->RealTimeSeconds`.

While armed, `UCrCharacterMovementComponent::TickComponent` (`0x1474DBC10`) calls
`UpdateTeleportState(false)` every frame. It counts `4 -> 1`, then each tick checks:

- `UWorldPartitionSubsystem::IsStreamingCompleted` within
  `Chimera.Movement.PostTeleportBuildingsOffLODSearchDistance` (default `5000.0` uu), and
- that no nearby `FCrMassBuildingTag` entities are still un-visualised.

Until those are satisfied it calls `DisableMovement()` and sets
`ULoadingScreenManager::bPlayerWaitingForWorldStreaming = 1` -- which is why the loading screen
says *"Player controller is waiting for world streaming"*.

So the protection is real. The bug is that it releases early.

### Relevant `UCrCharacterMovementComponent` offsets

| Offset | Field |
|---|---|
| `0x1318` | `bIgnoreOnPostTeleportStateChecks` |
| `0x1320` | `TeleportFinalFloorImpactPoint` |
| `0x1338` | `TeleportFinalFloorName` |
| `0x1340` | `TeleportRequiredEntityToHaveActorVisualization` |
| `0x1358` | `TeleportRequiredEntityPersistentIDToHaveActorVisualization` |
| `0x1368` | `bWaitForMassEntityToUnlockMovementAfterTeleport` |
| `0x136C` | `TeleportWaitingForLevelStreaming` |
| `0x1370` | `TeleportTimestamp` |

---

## Why it breaks

### 1. The 30 second hard give-up

| CVar | Type | Default |
|---|---|---|
| `Chimera.Movement.PostTeleportTimeoutDuration` | float | **30.0** |
| `Chimera.Movement.PostTeleportBuildingsOffLODSearchDistance` | float | 5000.0 |
| `Chimera.Movement.PostTeleportCollisionsDebug` | int | 0 |

Once `RealTimeSeconds - TeleportTimestamp > 30.0`, `UpdateTeleportState` bails out
**unconditionally**, with no check that the world is actually there:

```
UWorld::FindTeleportSpot
  -> SetActorLocation
  -> clear all wait state
  -> bPlayerWaitingForWorldStreaming = 0    (loading screen drops)
  -> SetMovementMode(MOVE_Walking)          (physics on)
```

Physics is now enabled with nothing underneath you. **That is symptom 1.**

### 2. The success test is a 10 cm exact-position match

For the ordinary (non-Mass) floor case, the only path that releases the hold *successfully* is:

```c
FindFloor(...);
v119 = |CurrentFloor.ImpactPoint - TeleportFinalFloorImpactPoint|;
if (v119 < 10.0)   // ten unreal units. one tenth of a metre.
    release;
```

If the floor you saved on comes back even slightly different -- landscape LOD, a rebuilt ISM
instance, a building whose Mass entity re-registered at a proxy transform -- that comparison can
*never* succeed. You then ride to the 30 second timeout on **every single load**.

This is what turns cause 1 from a rare race into a reproducible, save-specific bug.

### 3. Symptom 2 is the Mass path failing

If you saved standing on a platform, machine or habitat, releasing the hold requires
`UMassActorSubsystem::GetActorFromHandle(savedPersistentEntityID)` to return a real actor with
`FMassRepresentationLODFragment < 3`.

If that persistent ID no longer resolves -- entity recreated, building upgraded or deconstructed,
Mass save restored in a different order -- the condition is unsatisfiable. 30 seconds later you are
dumped onto the terrain, and the building's actor visualisation spawns moments after that at the
saved location, on top of you.

### 4. Remote clients may get no hold at all

`ClientSetTeleportDestinationFloorData_Implementation` and `...FloorEntityID_Implementation` only
write the floor fields -- neither sets `TeleportWaitingForLevelStreaming`. Arming also requires the
controller to be an `ACrPlayerControllerBase` that passes
`APlayerController::IsStreamingSourceEnabled()` (vtable `+0xC20`).

If both gates fail, the player has *zero* protection rather than 30 seconds of it. Worth checking
specifically on dedicated-server joins.

---

## The clock is worse than it looks

Three things about that 30 seconds compound:

**It runs during the loading screen.** In fact the loading screen exists *because* the timer is
running. `UpdateTeleportState` is what raises `bPlayerWaitingForWorldStreaming` each tick, and it
is driven from `TickComponent` -- so the world is live and ticking the whole time. The blocking
part of the load is already over when the hold is armed, because arming happens off
`PostLoadMapWithWorld`, which fires at the *end* of `LoadMap`.

**It is wall clock, and immune to pause.** From `UWorld::Tick` (`0x144CB4280`):

```asm
addsd xmm0, [rdi+848h]   ; RealTimeSeconds += DeltaRealTime   <- unconditional
movsd [rdi+848h], xmm0
test  r12b, r12b         ; paused?
jnz   short loc_144CB4545
addsd xmm1, [rdi+850h]   ; AudioTimeSeconds += ...            <- skipped when paused
```

`TimeSeconds` and `AudioTimeSeconds` are gated on pause and dilation. `RealTimeSeconds` at `+0x848`
is added to every frame regardless -- and that is the field `TeleportTimestamp` is captured from
and compared against. Pausing buys you nothing.

**Hitches are charged in full.** The delta added is the true frame delta, not a clamped one.
Streaming a large base produces multi-second frame hitches, and each one is billed against the
budget. A load that hitches through 12 seconds of stalls has burned 40% of its 30 seconds in a
dozen frames. So heavy streaming actively accelerates its own deadline -- a big base is both the
thing that takes longest to stream and the thing that hitches hardest doing it.

Minor note: `UCrSaveSubsystem::OnPostLoadMap` restores `World->RealTimeSeconds` from
`SaveData.WorldRealTimeSeconds` before broadcasting `OnSaveLoaded`, so the value accumulates across
sessions rather than starting near zero. The subtraction is still correct (the timestamp is
captured after the restore), but `TeleportTimestamp` is a `float` narrowed from the world's
`double` (`cvtpd2ps` at `0x1474DD81F`). At realistic magnitudes the precision loss is well under a
second.

---

## Diagnosing it: the game's own debug output

Setting `Chimera.Movement.PostTeleportCollisionsDebug` to `1` makes `UpdateTeleportState` log which
branch it took. There are 20 messages, all `LogTemp` / `Warning`, from
`CrCharacterMovementComponent.cpp` lines 2672-3023. `Warning` passes the default filter, so they
land in `%LOCALAPPDATA%\StarRupture\Saved\Logs\StarRupture.log` with no extra log configuration.

The leading `%d` on most of them is `OwnerPrivate->Role == ROLE_Authority` -- `1` on
server/standalone, `0` on a client. Handy for multiplayer triage.

### Arming and the streaming wait

| Line | Message |
|---|---|
| 2672 | `%d UCrCharacterMovementComponent Init Teleport timestamp: %f` |
| 2694 | `%d UCrCharacterMovementComponent Query Location: %s` |
| 2710 | `%d UCrCharacterMovementComponent Streaming not completed yet` |
| 2749 | `UCrCharacterMovementComponent - Standing on a ladder, unlock movement to flying mode.` |

### The Mass-building path (symptom 2)

| Line | Message |
|---|---|
| 2763 | `UCrCharacterMovementComponent - Waiting for an Entity to unlock movement` |
| 2778 | `UCrCharacterMovementComponent - Entity checkec from a persistent ID` |
| 2826 | `UCrCharacterMovementComponent - Mass Actor is Valid: %s Unlock movement. Extent %s` |
| 2840 | `UCrCharacterMovementComponent - Mass Actor is Valid: %s Don't unlock movement. Extent %s` |
| 2857 | `UCrCharacterMovementComponent - Mass Actor is not valid for entity: %d Waiting for actor to unlock movement after teleport.` |

(`checkec` is the developers' typo, not ours -- it makes a nice unambiguous grep anchor.)

### The no-floor-data path

| Line | Message |
|---|---|
| 2875 | `%d UCrCharacterMovementComponent there is entity in radius, floor distance: %f` |
| 2881 | `%d UCrCharacterMovementComponent Seems like there is a valid floor, enable walking` |
| 2895 | `%d UCrCharacterMovementComponent Seems like there is a valid floor but we are inside of it` |
| 2924 | `%d UCrCharacterMovementComponent Teleport, Custom hit distance: %f, Comp: %s` |
| 2932 | `%d UCrCharacterMovementComponent Teleport custom trace found floor, so will enable walk` |
| 2940 | `%d UCrCharacterMovementComponent Teleport, Dist to wanted floor: %f is larger than 200.0f so we will wait` |

Line 2875's wording is a copy-paste leftover -- it is logged on the no-floor-data path as a plain
floor-distance readout, with no entity radius check anywhere near it. Do not read meaning into it.

### The decision block -- this is where the answer is

| Line | Message |
|---|---|
| 2956 | `%d UCrCharacterMovementComponent Teleport, Dist to wanted floor: %f` |
| 2963 | `%d UCrCharacterMovementComponent Teleport waiting time: %f required: %f` |
| 2973 | `%d UCrCharacterMovementComponent Enable walking because it's fine` |
| 2983 | `%d UCrCharacterMovementComponent Teleport good saved floor is proper, should unlock` |
| 3008 | `%d UCrCharacterMovementComponent Enable walking because timeout` |
| 3023 | `%d UCrCharacterMovementComponent Teleport Disable movement` |

**`Enable walking because timeout` (line 3008) is the smoking gun.** It is the only path that
reaches `FindTeleportSpot` + `MOVE_Walking` without the world being ready. The healthy exits are
2973, 2983, 2881 and 2895. Exactly one of those six fires per load, so a single grep tells you
which branch you landed in:

```bash
grep -nE "Enable walking|should unlock|valid floor|Init Teleport timestamp" StarRupture.log
```

Two things to expect:

- **It is loud.** `Query Location` (2694) and `Teleport waiting time` (2963) print *every tick*
  while the hold is active -- roughly 1800 lines each over a 30 second wait at 60fps. That is
  genuinely useful (`waiting time: %f required: %f` is a live readout of the budget draining, and
  lets you count real ticks against hitches) but you want it on only for the repro.
- **If `Init Teleport timestamp` never appears at all**, the hold was never armed. That is cause 4
  above -- a different failure from the timeout, and it means the player had no protection
  whatsoever.

---

## Setting the CVar

This is the awkward part, and it is why this document is longer than "set a CVar".

### What does not work

Both of the normal out-of-process routes are **dead in the shipping build**. This was tested, not
assumed:

- **`Engine.ini` is not read.** Adding a `[ConsoleVariables]` section to
  `%LOCALAPPDATA%\StarRupture\Saved\Config\Windows\Engine.ini` has no effect. Neither does
  `[Core.Log]`, and neither does an unrelated control such as `r.ScreenPercentage=50`. Two
  independent settings with zero effect is the file not being combined, not a syntax problem. The
  install ships `LogConfig: Using compiled CustomConfig GameSteam`, a 3-byte BOM-only
  `Engine/Config/StagedBuild_StarRupture.ini` marker, and no loose `StarRupture/Config/` directory
  at all -- the config hierarchy is baked in. (`Saved/Config/Windows/` *is* still the generated
  config dir; `GameUserSettings.ini` works there. The `Engine.ini` layer specifically is not
  applied.)
- **Command line CVar injection does not exist.** The binary contains no `-ExecCmds`, `-DPCVars`
  or `-cvarsfromcmdline` token. The only near-hit is `InOutExecCmds`, a function parameter name.

For the avoidance of doubt, logging is *not* compiled out -- that was the other obvious hypothesis
and it is wrong. `LogTemp`'s constructor at `0x140DDF260` passes compile-time verbosity
`7 (VeryVerbose)` and default runtime verbosity `5 (Log)`; the gate in `UpdateTeleportState` is
`cmp cs:LogTemp.Verbosity, 3` (Warning), which passes with room to spare. The `call BasicLog`
instructions are physically present at all 20 sites, and `LogTemp: Error` lines do reach the log
file. The only closed gate is the CVar itself being `0`.

### What does work: in-process

`UpdateTeleportState` does not query the console manager -- it reads the cached shadow value
directly:

```c
v112 = CVarPostTeleportTimeoutDuration.Ref->ShadowedValue[0];
if (CVarPostTeleportCollisionsDebug.Ref->ShadowedValue[0] && LogTemp.Verbosity >= Warning) ...
```

So anything already inside the process can set these with two stores -- no `IConsoleManager`, no
name lookup, no console access. The three `TAutoConsoleVariable` objects are contiguous in `.data`:

| Object | VA | RVA |
|---|---|---|
| `CVarPostTeleportTimeoutDuration` | `0x14E426EA0` | `0xE426EA0` |
| `CVarPostTeleportCollisionsDebug` | `0x14E426EB8` | `0xE426EB8` |
| `CVarPostTeleportBuildingsOffLODSearchDistance` | `0x14E426ED0` | `0xE426ED0` |

Object layout is 24 bytes: `+0` vtable, `+8` `Target` (`IConsoleVariable*`), `+16` `Ref`
(`TConsoleVariableData<T>*`). `ShadowedValue[0]` is the `T` at `Ref+0`.

```c
// CU1, imagebase 0x140000000.
*(int32_t*)(*(void**)(base + 0xE426EB8 + 16)) = 1;      // CollisionsDebug -> on
*(float*)  (*(void**)(base + 0xE426EA0 + 16)) = 180.0f; // TimeoutDuration -> 180s
```

Write `[Ref+4]` as well to cover the render-thread copy. The dynamic initializers run before
`WinMain`, so `Ref` is already populated by the time an injected DLL attaches.

Rather than hardcoding RVAs, anchor on one of the three and use the fixed `0x18` spacing -- they
are always in the order Timeout, CollisionsDebug, BuildingsOffLOD.

### Or: the ModLoader's developer console

If you are running the StarRupture ModLoader, its ImGui developer console (tilde) executes commands
through `APlayerController::ConsoleCommand`, so you can just type:

```
Chimera.Movement.PostTeleportTimeoutDuration 180
```

**Caveat:** this needs a live local player controller, which means you can only do it *after* you
have loaded in -- too late to help the load that is currently happening. CVar values persist for
the lifetime of the process though, so setting it once protects every subsequent save load in that
session. For protection on the *first* load it has to be a direct memory write during attach, per
above.

---

## Appendix: binary patching

Possible, and a smaller patch than the save-size one, but it is the weakest of the options: Steam's
*Verify integrity of game files* reverts it, and so does every game update. Documented here for
dedicated-server operators who do not inject anything.

**Do not patch the constant.** The `30.0` lives at `0x14AEF5130` as `__real@41f00000`, and that is
a COMDAT-folded literal **shared by 74 call sites** across the binary -- Nanite pixel scaling,
ray-tracing traversal scale, fog start distance, menu tooltip timing, audio spectral analysis,
`LoadPackageInternal`, checkpoint upload delay. Changing those four bytes changes all of them.

Patch the **instruction's displacement** instead, so only this CVar's initializer reads elsewhere.

`_dynamic_initializer_for__CVarPostTeleportTimeoutDuration__` at `0x1410AD160`. Raw bytes from
`0x1410AD1AC`:

```
4C 8D 0D ED 0D D4 09        lea    r9,  pwszOutputURL
F3 0F 10 15 75 7F E4 09     movss  xmm2, [rip+0x9E47F75]   <-- the 30.0f default
48 8D 15 9E EA 59 0B        lea    rdx, "Chimera.Movement.PostTeleportTimeoutDuration"
C7 44 24 20 00 00 00 00     mov    [rsp+20h], 0            ; flags
```

Four bytes change, at VA `0x1410AD1B7`. `rip` = `0x1410AD1BB`, so
`disp32 = targetVA - 0x1410AD1BB`. Round-trip check: `0x1410AD1BB + 0x09E47F75 = 0x14AEF5130`,
which holds `00 00 F0 41` = 30.0f.

Repoint it at an existing float already sitting in `.rdata`:

| Timeout | Constant VA | Value bytes | New disp32 | Patch bytes |
|---|---|---|---|---|
| 30 s *(original)* | `0x14AEF5130` | `00 00 F0 41` | `0x09E47F75` | `75 7F E4 09` |
| 120 s | `0x14AE2DFCC` | `00 00 F0 42` | `0x09D80E11` | `11 0E D8 09` |
| 300 s | `0x14AFA28B8` | `00 00 96 43` | `0x09EF56FD` | `FD 56 EF 09` |
| 1000 s | `0x14ADF6C18` | `00 00 7A 44` | `0x09D49A5D` | `5D 9A D4 09` |

Aliasing an unrelated constant is safe because the site only ever *loads* 4 bytes from that address
and `.rdata` is read-only. It does not matter what else those bytes mean to other code -- nothing
writes there. A patcher does not need a named constant; it can scan `.rdata` for the 4 bytes of the
desired float, 4-byte aligned, and take the first hit.

To survive game updates, do not anchor on the instruction shape -- `lea r9 / movss xmm2 / lea rdx /
mov [rsp+20h],0` is what *every* float CVar initializer in the binary looks like. Anchor on the
name string:

1. Find UTF-16LE `Chimera.Movement.PostTeleportTimeoutDuration\0` in the file; convert file offset
   to VA via the section headers.
2. Scan `.text` for `48 8D 15 <rel32>` where `nextInsnVA + rel32 == stringVA`. Expect exactly one
   hit; bail if not.
3. Step back 8 bytes and require `F3 0F 10 15`.
4. Resolve its current target and read the float there. This is both the verification and the state
   readout -- you can display "currently: 30 s" and re-patch idempotently to any value.
5. Scan `.rdata` for the desired float, aligned, and write `newTargetVA - (movssVA + 8)`.

Since `RegisterConsoleVariable` only takes 30.0 as the *default*, all three mechanisms compose --
an in-process write or a console command still overrides a patched binary, so patching locks
nothing in.

---

## Fix strategies, ranked

**1. Raise the timeout.** Write `Chimera.Movement.PostTeleportTimeoutDuration = 180` in-process at
attach. This alone converts "dropped into the void after 30 s" into "the loading screen stays up
until the world is actually there". One store, no hooks.

Downside: a genuinely unsatisfiable condition (causes 2 and 3) now hangs on the loading screen
instead of killing you. Pair it with a safety net.

**2. Safety net, no AOB required.** Cache the save's `WorldTransform` on load, and for the first
~60 seconds watch the local pawn. If it drops more than N metres below the saved Z, or is in
`Falling` with no floor, teleport it straight back. Crude, but it catches every variant --
including the ones the engine's own logic cannot express.

**3. The real fix, if you want to maintain an AOB.** Hook
`UCrCharacterMovementComponent::UpdateTeleportState` (`0x1474DD760`) and replace the give-up
branch: instead of `FindTeleportSpot` + `MOVE_Walking`, re-teleport to the saved transform, re-arm
the hold, and only surrender once a downward sweep actually finds solid ground.

Do 1 and 2 first, get `PostTeleportCollisionsDebug` output from a real repro, then decide whether 3
is worth the maintenance burden.

---

## Provenance

All addresses and offsets were read from the **CU1 client build**
(`StarRuptureGameSteam-Win64-Shipping.exe`, imagebase `0x140000000`, Jun-17). An earlier January
Early Access build does **not** match -- if you are following along in IDA, check your imagebase
and spot-check `UpdateTeleportState` at `0x1474DD760` before trusting any address here. The
dedicated server executable needs its own addresses; the logic is the same, the offsets are not.
