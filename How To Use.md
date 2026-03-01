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
   ├── version.dll                ← mod loader
   └── alienx_mods\
       ├── KeepTicking.dll
       ├── RailJunctionFixer.dll
       └── ...
   ```

3. Launch the game (or server) as normal. The mod loader will initialise automatically via `version.dll`.

Note for Linux Users
In order for the mod to load, you must have the envrionment variable `WINEDLLOVERRIDES` set with the value `version=n,b`

---

## Enabling Plugins

**Plugins are disabled by default.** The first time the game runs with the mod loader present, config files are generated for each plugin. To enable a plugin:

1. Run the game once and then close it.
2. Open the following folder:
   ```
   StarRupture\Binaries\Win64\alienx_mods\config\
   ```
3. Open the `.ini` file for the plugin you want to enable (e.g. `KeepTicking.ini`).
4. Set `Enabled=1` and save the file.
5. Launch the game again — the plugin will now be active.

Repeat for each plugin you want to use.
