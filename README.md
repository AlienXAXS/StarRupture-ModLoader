# StarRupture ModLoader
[![Build and Release](https://github.com/AlienXAXS/StarRupture-ModLoader/actions/workflows/release.yml/badge.svg)](https://github.com/AlienXAXS/StarRupture-ModLoader/actions/workflows/release.yml)

A plugin-based mod loader for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/). Loads automatically via DLL proxy injection — no game files are modified.

Like this project? Give it a star here on GitHub!

---

## Discord

[Join our Discord server](https://discord.gg/QUzsGKe5Bz) to chat about the mod loader and plugins.

---

## Installation

### 1. Download the right ZIP

Two builds are published on the [releases page](https://github.com/AlienXAXS/StarRupture-ModLoader/releases):

| ZIP | Use for |
|---|---|
| `StarRupture-ModLoader-Client-*.zip` | Playing the game |
| `StarRupture-ModLoader-Server-*.zip` | Running a dedicated server |

### 2. Extract into the game's binary folder

That's the folder containing `StarRuptureGameSteam-Win64-Shipping.exe`, for example:

```
StarRupture\Binaries\Win64\
```

The ZIP is already laid out correctly, so extract it **directly into that folder** — no subfolders
need creating by hand. Afterwards you should see:

```
StarRupture\Binaries\Win64\
├── StarRuptureGameSteam-Win64-Shipping.exe
├── dwmapi.dll                        ← the proxy the game loads
└── ModLoader\
    ├── StarRupture-ModLoader-Core.dll   ← the mod loader itself
    ├── StarRupture-ImGui.dll            ← client builds only
    ├── modloader.ini                    ← created on first launch
    ├── Logs\                            ← created on first launch
    └── Plugins\                         ← put plugin DLLs here
```

Everything the mod loader owns lives under `ModLoader\`; `dwmapi.dll` is the only file that sits
next to the game executable.

### 3. Launch as normal

The mod loader initialises automatically via `dwmapi.dll`. No game files are modified.

**Linux users:** the mod will not load unless the environment variable `WINEDLLOVERRIDES` is set to
`dwmapi=n,b`.

---

## Installing Plugins

The mod loader ships with no plugins — download the ones you want and drop their `.dll` into
`Binaries\Win64\ModLoader\Plugins\`. Client plugins only load on a client, server plugins only on
a dedicated server; the mod loader window says so if you get one the wrong way round.

**Plugins are disabled by default.** The first time the game runs with a plugin present, a config
file is generated for it. To enable it:

1. Run the game once, then close it.
2. Open `Binaries\Win64\ModLoader\Plugins\config\`.
3. Open the `.ini` named after the plugin.
4. Set `Enabled=1` and save.
5. Launch again — the plugin is now active.

Repeat for each plugin you want to use. On a client you can do all of this from the mod loader
window instead, without editing files.

If a plugin ships a `.json` file alongside its `.dll`, it can update itself: the mod loader checks
for a new version at startup and downloads it. The **Plugins** tab marks those with a green dot
(amber means the plugin has no update manifest and has to be updated by hand). Auto-updating can be
turned off in the **Settings** tab.

---

## The Mod Loader Window (client)

Press **F2** in game to open it. Down the left is an icon for each screen:

| Screen | What it's for |
|---|---|
| **Plugins** | Load, unload and reload installed plugins without restarting |
| **Config** | Edit each plugin's own settings and keybinds |
| **Settings** | HUD overlays (FPS, world name, position), auto-updates, diagnostics |
| **Logging** | Log levels for the mod loader, the game, and each plugin — see below |
| **Theme** | Font, text size and every UI colour |
| **About** | Build tag and the plugins currently loaded |

A developer console is on **`~`** (tilde). Both keys can be changed in
`ModLoader\modloader.ini` — `[UI] OpenKey` and `[Console] OpenKey`.

---

## The Server Console (dedicated server)

A dedicated server has no UI, so launch it with `-console` for a console window with the same
commands. `help` lists them; `plugins`, `reload`, `load`, `unload` and `rescan` manage plugins
without restarting the server, and `stop` shuts it down cleanly.

---

## Log Levels

The mod loader writes to `Binaries\Win64\ModLoader\Logs\ModLoader.log`, keeping the last ten runs.
Open the mod loader window and go to the **Logging** tab to change how much detail it records:

- **Log Level** — the mod loader's own output. Saved, so it applies on every launch.
- **Game Log Verbosity** — the game's own log categories, written to `StarRupture.log`. Also saved.
- **Per plugin** — a grid with one row per plugin. Turn a single plugin up to read its output
  without the rest of the log burying it, or down to silence a noisy one without quieting
  everything else. `Default` means "follow the Log Level above".

Per-plugin levels are **not saved** — every plugin is back on `Default` next launch.

### Setting a plugin's level before the game starts

A plugin that logs heavily while the game is still loading has already filled the log by the time
you can open the Logging tab. Add a launch option to have the level in place before it loads.
In Steam: right-click the game, *Properties* → *General* → *Launch Options*.

```
-PluginLogLevel=<plugin>:<level>
```

`<plugin>` is the name as it appears in `[Plugin:NAME]` in the log — the same name the Logging tab
shows. `<level>` is `trace`, `debug`, `info`, `warn`, `error`, or `default`.

Several at once, comma separated:

```
-PluginLogLevel=NoisyPlugin:error,OtherPlugin:trace
```

`*` sets the level for every plugin you have not named:

```
-PluginLogLevel=*:warn,PluginImDebugging:trace
```

Quote the whole option if a plugin name contains a space:

```
-PluginLogLevel="My Plugin:error"
```

The Logging tab shows when a launch option is in effect, and *Reset All To Default* clears it for
the rest of the session.

---

## Developing Your Own Plugins

Use the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) — it has everything you need (headers, UE5 SDK, an example plugin, and a pre-built `dwmapi.dll`) without requiring a fork of this repo.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Nothing loads at all | `dwmapi.dll` must sit next to the game `.exe` with the `ModLoader\` folder beside it — not in a subfolder of its own. On Linux, set `WINEDLLOVERRIDES=dwmapi=n,b`. |
| Plugins not loading | Make sure the DLLs are in `ModLoader\Plugins\` and `Enabled=1` is set in each plugin's `.ini`. The **Plugins** tab shows why a plugin was rejected. |
| "Cannot Load" next to a plugin | It was built for the other target — a server plugin on a client, or the reverse. |
| "Needs Update" next to a plugin | The plugin and the mod loader were built against different plugin interface versions. The tab says which of the two to update. |
| Logs / diagnostics | Check `ModLoader\Logs\ModLoader.log`. Raise the detail in the **Logging** tab, or set `Level=DEBUG` under `[Logging]` in `ModLoader\modloader.ini`. |
| One plugin is flooding the log | See [Log Levels](#log-levels) — turn that plugin down on its own, or use `-PluginLogLevel=` if it happens during startup. |
| Game stops responding to input | Turn on *ModLoader Debug Values* in the **Settings** tab — it names the plugin holding input open. |

---

## Author

**AlienX** — [GitHub](https://github.com/AlienXAXS) · [AGNGaming](https://www.agngaming.com)

## Credits

- **[Dumper-7](https://github.com/Encryqed/Dumper-7)** — Unreal Engine SDK generation
- **[MinHook](https://github.com/TsudaKageyu/minhook)** — Function hooking library
- **[nlohmann/json](https://github.com/nlohmann/json)** — JSON parsing

---

## Tools Used

- **[IDA Pro](https://hex-rays.com/)** by Hex-Rays — reverse engineering the game binary to find hook targets and AOB patterns
- **[Claude Code](https://claude.com/claude-code)** — AI coding assistant used throughout development

---

## Disclaimer

This is a modding tool for educational purposes. Use at your own risk. The authors are not responsible for any damage caused by using this software. While every effort is made to ensure save file compatibility across updates, this cannot be guaranteed.

---

**Game:** Star Rupture · **Engine:** Unreal Engine 5
