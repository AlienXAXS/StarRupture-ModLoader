# RuptureTimer

Tracks the rupture wave timer and makes the data available in two ways:

- **JSON export** — writes a file every second that external tools (StreamDeck, overlays, scripts) can read.
- **In-game HUD overlay** — optional plain-text overlay drawn directly on screen showing the three key values at a glance.

---

## What it tracks

| Value | Description |
|-------|-------------|
| **Next Rupture** | Countdown until the next wave hits. Shows `NOW` while a wave is active. |
| **Planet Status** | Current phase of the planet: Stable, Warning, Burning, Cooling, or Stabilizing. Includes wave type (Heat / Cold) when a wave is in progress. |
| **Wave Timer** | Time remaining in the current phase. |

The tracker works across all connection types — local play, listen server, and dedicated server clients. Detection accuracy varies by mode:

- **Local / listen server** — full accuracy, reads phase progress directly from the game.
- **Dedicated server client** — phase name from replicated actor; timing from server timestamp.
- **Fallback** — phase inferred from observable jumps in the server timestamp (accurate for next-rupture countdown, approximate for current phase label).

---

## Installation

Drop `TimerMod.dll` into:

```
steamapps/common/StarRupture/StarRupture/Binaries/Win64/Plugins/
```

On first run the mod loader generates the config file. Restart (or edit the file and reload) to apply changes.

---

## Configuration

Config file location:

```
steamapps/common/StarRupture/StarRupture/Binaries/Win64/Plugins/config/RuptureTimer.ini
```

---

### [General]

```ini
[General]
; Enable or disable the plugin entirely
Enabled=1
```

---

### [Export] — JSON file for external tools

```ini
[Export]
; Write timer state to a JSON file (set to 0 to disable)
WriteJsonFile=1

; Path to the output file, relative to the game directory
JsonFilePath=Plugins/data/rupture_timer.json

; How often the file is updated (seconds, minimum 0.1)
UpdateIntervalSeconds=1.0

; Include per-phase breakdown timers in the JSON output.
; Only populated in full mode (local / listen server).
; Adds: warning_remaining_sec, burning_remaining_sec, cooling_remaining_sec,
;       stabilizing_remaining_sec, stable_remaining_sec
ExtendedPhaseTimers=0
```

#### JSON output format

Standard output (`ExtendedPhaseTimers=0`):

```json
{
  "valid": true,
  "phase": "Stable",
  "phase_remaining_sec": 2550.0,
  "next_rupture_in_sec": 2550.0,
  "wave_number": 3,
  "wave_type": "None",
  "paused": false
}
```

Extended output (`ExtendedPhaseTimers=1`):

```json
{
  "valid": true,
  "phase": "Warning",
  "phase_remaining_sec": 12.4,
  "next_rupture_in_sec": 12.4,
  "wave_number": 4,
  "wave_type": "Heat",
  "paused": false,
  "warning_remaining_sec": 12.4,
  "burning_remaining_sec": 30.0,
  "cooling_remaining_sec": 60.0,
  "stabilizing_remaining_sec": 600.0,
  "stable_remaining_sec": null
}
```

`null` means the value is not yet known (e.g. the next stable period hasn't been scheduled yet). When `valid` is `false` the file contains only `{ "valid": false }`.

**Phase values:** `Stable` · `Warning` · `Burning` · `Cooling` · `Stabilizing` · `Unknown`

**Wave type values:** `None` · `Heat` · `Cold`

---

### [HUD] — In-game text overlay

```ini
[HUD]
; Show the rupture timer as a text overlay in-game (0 = off, 1 = on)
ShowOverlay=0

; Anchor position on screen (see position table below)
Position=LowerLeft

; Text scale multiplier (1.0 = default engine font size)
Scale=1.0
```

The overlay draws three lines:

```
Next Rupture: 42:30
Planet: Stable
Wave Timer: 42:30
```

While a wave is active:

```
Next Rupture: NOW
Planet: Burning (Heat)
Wave Timer: 0:28
```

#### HUD positions

| Value | Location |
|-------|----------|
| `TopLeft` | Top-left corner |
| `TopMid` | Top edge, centred horizontally |
| `TopRight` | Top-right corner |
| `MidLeft` | Left edge, vertically centred |
| `MidRight` | Right edge, vertically centred |
| `LowerLeft` | Bottom-left corner *(default)* |
| `LowerRight` | Bottom-right corner |
