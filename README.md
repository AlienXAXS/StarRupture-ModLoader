# StarRupture ModLoader
[![Build and Release](https://github.com/AlienXAXS/StarRupture-ModLoader/actions/workflows/release.yml/badge.svg)](https://github.com/AlienXAXS/StarRupture-ModLoader/actions/workflows/release.yml)

A plugin-based mod loader for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/). Loads automatically via DLL proxy injection — no game files are modified.

Like this project? Give it a star here on GitHub!

---

## Discord

[Join our Discord server](https://discord.gg/QUzsGKe5Bz) to chat about the mod loader and plugins.

---

## Installation

> **Full step-by-step instructions are in [How To Use](How%20To%20Use.md).**

**Quick version:**

1. Download the latest release ZIP for your use case:
   - `StarRupture-ModLoader-Client-*.zip` — for playing the game
   - `StarRupture-ModLoader-Server-*.zip` — for running a dedicated server

2. Extract into your game's `Binaries\Win64\` folder (where the `.exe` lives).

3. Launch the game or server as normal.

4. **Plugins are disabled by default.** After the first launch, edit the `.ini` files in `Plugins\config\` and set `Enabled=1` for each plugin you want.

**Linux users:** Set the environment variable `WINEDLLOVERRIDES=dwmapi=n,b` before launching.

---

## Plugins

| Plugin | Target | Description |
|---|---|---|
| [KeepTicking](https://github.com/AlienXAXS/StarRupture-Plugin-KeepTicking) | Server | Prevents a dedicated server from sleeping when no players are online |
| [ServerUtility](https://github.com/AlienXAXS/StarRupture-Plugin-ServerUtility) | Server | Command-line server settings, Source RCON, Steam A2S Query, and remote vulnerability patch |
| [Compass](https://github.com/AlienXAXS/StarRupture-Plugin-Compass) | Client | Adds a HUD compass bar showing nearby players, bases, markers, and points of interest |

---

## Developing Your Own Plugins

Use the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) — it has everything you need (headers, UE5 SDK, an example plugin, and a pre-built `dwmapi.dll`) without requiring a fork of this repo.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Plugins not loading | Make sure DLLs are in the `Plugins\` folder and `Enabled=1` is set in each plugin's `.ini` file. |
| RCON won't start | Both `-RconPort=` and `-RconPassword=` must be provided on the command line. |
| Server sleeps when empty | Enable KeepTicking in `Plugins\config\KeepTicking.ini`. |
| Logs / diagnostics | Check `modloader.log` for detailed output. Enable debug logging by setting `Level=DEBUG` in `modloader.ini`. |

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
