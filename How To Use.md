# How To Use — StarRupture ModLoader

The StarRupture ModLoader is a plugin-based mod framework that loads automatically when you launch the game.

---

## Choosing the Right ZIP

Two builds are available on the releases page:

| ZIP | Use for |
|-----|---------|
| `StarRupture-ModLoader-Client-*.zip` | Playing the game (client) |
| `StarRupture-ModLoader-Server-*.zip` | Running a dedicated server |

Download the one that matches your use case.

---

## Installation

1. Locate your game binary folder — this is the folder that contains `StarRuptureGameSteam-Win64-Shipping.exe`, for example:
   ```
   StarRupture\Binaries\Win64\
   ```

2. Extract the contents of the ZIP **directly into that folder**. The ZIP is already laid out correctly, so no subfolders need to be created manually. After extraction you should see:
   ```
   StarRupture\Binaries\Win64\
   ├── StarRuptureGameSteam-Win64-Shipping.exe
   ├── dwmapi.dll                 ← mod loader
   └── Plugins\
       ├── ExamplePlugin1.dll     ← Just example plugin names.
       ├── ExamplePlugin2.dll
       └── ...
   ```

3. Launch the game (or server) as normal. The mod loader will initialise automatically via `dwmapi.dll`.

Note for Linux Users
In order for the mod to load, you must have the envrionment variable `WINEDLLOVERRIDES` set with the value `dwmapi=n,b`

---

## Enabling Plugins

**Plugins are disabled by default.** The first time the game runs with the mod loader present, config files are generated for each plugin. To enable a plugin:

1. Run the game once and then close it.
2. Open the following folder:
   ```
   StarRupture\Binaries\Win64\Plugins\config\
   ```
3. Open the `.ini` file for the plugin you want to enable (e.g. `KeepTicking.ini`).
4. Set `Enabled=1` and save the file.
5. Launch the game again — the plugin will now be active.

Repeat for each plugin you want to use.

---

## Log Levels

The mod loader writes to `StarRupture\Binaries\Win64\ModLoader\Logs\ModLoader.log`. Open the mod
loader window and go to the **Logging** tab to change how much detail it records:

- **Log Level** — the mod loader's own output. Saved, so it applies on every launch.
- **Game Log Verbosity** — the game's own log categories, written to `StarRupture.log`. Also saved.
- **Per plugin** — a grid with one row per plugin. Turn a single plugin up to read its output
  without the rest of the log burying it, or down to silence a noisy one without quieting
  everything else. `Default` means "follow the Log Level above".

Per-plugin levels are **not saved** — every plugin is back on `Default` next launch.

### Setting a plugin's level before the game starts

A plugin that logs heavily while the game is still loading has already filled the log by the time
you can open the Logging tab. Add a launch option to have the level in place before it loads.
In Steam: right-click the game, *Properties* -> *General* -> *Launch Options*.

```
-PluginLogLevel=<plugin>:<level>
```

`<plugin>` is the name as it appears in `[Plugin:NAME]` in the log — the same name the Logging tab
shows. `<level>` is `trace`, `debug`, `info`, `warn`, `error`, or `default`.

Several at once, comma separated:

```
-PluginLogLevel=NoisyPlugin:error,OtherPlugin:trace
```

`*` sets the level for every plugin that you have not named:

```
-PluginLogLevel=*:warn,PluginImDebugging:trace
```

Quote the whole option if a plugin name contains a space:

```
-PluginLogLevel="My Plugin:error"
```

The Logging tab shows when a launch option is in effect, and *Reset All To Default* clears it for
the rest of the session.
