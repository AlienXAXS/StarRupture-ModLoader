# Mod index

`index.json` is the list of ModLoader plugins the overlay's Browse tab shows. The loader
downloads it once, when the tab is first opened, and keeps it in memory.

## Add a plugin

Add one object to the array:

```json
{
  "id": "codex",
  "name": "Codex",
  "author": "AlienXAXS",
  "version": "1.2.0",
  "summary": "In-game recipe codex: search any item or recipe.",
  "page_url": "https://github.com/AlienXAXS/StarRupture-Plugin-Codex",
  "interface_version_min": 77
}
```

| Field | Needed | Notes |
|---|---|---|
| `id` | yes | Short, lowercase. Up to 64 characters. |
| `name` | yes | The name shown in the list, and the name the loader shows for the loaded plugin. Up to 64. |
| `page_url` | yes | Where the Browse tab sends the player. See the rule below. Up to 200 characters. |
| `author` | no | Up to 64 characters. |
| `version` | no | The latest release. Up to 32 characters. |
| `summary` | no | One or two sentences. Up to 300 characters. |
| `interface_version_min` | no | The plugin interface number the plugin declares. Leave it out when unknown. |

The list holds up to 200 plugins, and the file stays under 256 KB.

## The page_url rule

`page_url` must start with `https://` and the host must be exactly `nexusmods.com`,
`www.nexusmods.com`, `github.com` or `www.github.com`. No port, no user name, no backslash, no
spaces. An entry that breaks the rule is left out of the list, and so is an entry missing `id`,
`name` or `page_url`. The other entries still load.

## Use another index

Point the loader at your own file in `ModLoader\modloader.ini`:

```
[AutoUpdate]
ModIndexUrl=https://example.com/my-index.json
```

The URL must be https. The entries are checked by the same rules.
