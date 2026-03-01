# StarRupture ModLoader

A plugin-based mod loader for [Star Rupture](https://store.steampowered.com/app/1187400/Star_Rupture/). Loads automatically via DLL proxy injection — no game files are modified.

---

## Plugins

### 🔧 KeepTicking *(Server)*
Prevents a dedicated server from sleeping when no players are online. Without this, all of your machines will stop producing items.
*Note: Local vermin will still spawn until this plugin is updated*

### 🛤️ RailJunctionFixer *(Client & Server)*
Fixes rail junction save/load issues at runtime. Without this fix, rail junctions may lose their connections after loading a save causing logistic drones to teleport between rails.

### 🖥️ ServerUtility *(Server)*
Adds remote administration to a dedicated server:

- **Command-line server settings** — pass `-SessionName=`, `-SaveGameInterval=`, etc. to bypass `DSSettings.txt` entirely. Automatically detects existing saves and sets `StartNewGame` / `LoadSavedGame` accordingly.
- **Source RCON** — authenticated remote command execution (TCP) compatible with any standard RCON client.
- **Steam A2S Query** — server browser integration (UDP) for player counts, server name, and map info.

See the [RCON & Query documentation](ServerUtility/RCON_README.md) for protocol details, supported commands, and client compatibility.

### 📝 ExamplePlugin *(Template)*
A starter template for plugin development. Demonstrates the plugin lifecycle, config system, and logging — does not modify gameplay. See the [Developer Guide](DEVELOPERS.md) if you want to create your own plugins.

---

## Installation

> **Full step-by-step instructions are in [How To Use](How%20To%20Use.md).**

**Quick version:**

1. Download the latest release ZIP for your use case:
   - `StarRupture-ModLoader-Client-*.zip` — for playing the game
   - `StarRupture-ModLoader-Server-*.zip` — for running a dedicated server

2. Extract into your game's `Binaries\Win64\` folder (where the `.exe` lives).

3. Launch the game or server as normal.

4. **Plugins are disabled by default.** After the first launch, edit the `.ini` files in `alienx_mods\config\` and set `Enabled=1` for each plugin you want.

**Linux users:** Set the environment variable `WINEDLLOVERRIDES=version=n,b` before launching.

---

## ServerUtility — Command-Line Parameters

When running a dedicated server with ServerUtility enabled, these parameters replace `DSSettings.txt`:

| Parameter | Required | Description |
|---|---|---|
| `-SessionName=<name>` | Yes | Session/server name |
| `-SaveGameInterval=<seconds>` | No | Autosave interval (default: `300`) |
| `-RconPort=<port>` | No | TCP/UDP port for RCON and Steam Query |
| `-RconPassword=<password>` | No | Password for RCON authentication |

**Example:**
```
StarRuptureGameSteam-Win64-Shipping.exe -SessionName="My Server" -SaveGameInterval=600 -RconPort=27015 -RconPassword=secret
```

When `-SessionName=` is present, `DSSettings.txt` is not needed. The save game name is always `AutoSave0.sav`, and `StartNewGame` / `LoadSavedGame` are set automatically based on whether an existing save file is found.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Plugins not loading | Make sure DLLs are in the `alienx_mods\` folder and `Enabled=1` is set in each plugin's `.ini` file. |
| RCON won't start | Both `-RconPort=` and `-RconPassword=` must be provided on the command line. |
| Rail junctions still broken | Enable RailJunctionFixer in the plugins `.ini` file. |
| Server sleeps when empty | Enable KeepTicking in the plugins `.ini` file. |
| Logs / diagnostics | Check `version-mod.log` for detailed output.  Enable debug log level by setting `Level=DEBUG` in `version-mod.ini`. |

---

## Development

Want to create your own plugins or contribute? See the **[Developer Guide](DEVELOPERS.md)**.

---

## Credits

- **[Dumper-7](https://github.com/Encryqed/Dumper-7)** — Unreal Engine SDK generation
- **[MinHook](https://github.com/TsudaKageworker/minhook)** — Function hooking library
- **[nlohmann/json](https://github.com/nlohmann/json)** — JSON parsing
- **[Wilhelm-af](https://github.com/Wilhelm-af)** — Rail junction fix logic

## Disclaimer

This is a modding tool for educational purposes. Use at your own risk. The authors are not responsible for any damage caused by using this software.  While I will do everything possible to ensure that your save files will still work in future updates, I cannot be certain that your save files will always load.

---

**Game:** Star Rupture · **Engine:** Unreal Engine 5 · **Mod Loader Version:** 1.0.0
