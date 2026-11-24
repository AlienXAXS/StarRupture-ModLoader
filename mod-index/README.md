# Mod index

`index.json` is the list of ModLoader plugins the overlay's Browse tab shows. The loader
downloads it once, when the tab is first opened. The list players get is a signed copy of this file.

## Add a plugin

Add one object to the array. Send it the way you send rows for the version object map (the
shared per-game-build data the loader serves): open a PR.

```json
{
  "id": "codex",
  "name": "Codex",
  "author": "AlienXAXS",
  "version": "1.2.0",
  "summary": "In-game recipe codex: search any item or recipe.",
  "page_url": "https://github.com/AlienXAXS/StarRupture-Plugin-Codex",
  "readme_url": "https://raw.githubusercontent.com/AlienXAXS/StarRupture-Plugin-Codex/main/README.md",
  "tested_loader": "1.27.0",
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
| `readme_url` | no | A raw GitHub link to your README (host `raw.githubusercontent.com`, https). Please include it. |
| `tested_loader` | no | The ModLoader version you tested on, for example `1.27.0`. Up to 32 characters. Please include it. |
| `interface_version_min` | no | The plugin interface number the plugin declares. Leave it out when unknown. |

The list holds up to 200 plugins, and the file stays under 256 KB.

## Check it before you send it

Save `index.json` with LF line endings and a final newline, then run:

```
node .github/vmap/build-mod-index.mjs mod-index/index.json --out mod-index.txt
```

It prints `ok`, or names the entry that breaks a rule below.

## After it is merged

The maintainers sign the list and publish it as `mod-index.v1.txt` together with the version map;
no ModLoader release is needed. The loader checks the signature
before it reads an entry, and keeps its last good copy for when a download fails.

## The page_url rule

`page_url` must start with `https://` and the host must be exactly `nexusmods.com`,
`www.nexusmods.com`, `github.com` or `www.github.com`. No port, no user name, no backslash, no
spaces. An entry that breaks the rule is left out of the list, and so is an entry missing `id`,
`name` or `page_url`. The other entries still load.

## Try your own index

To test a list before it is merged, point the loader at your own file in `ModLoader\modloader.ini`:

```
[AutoUpdate]
ModIndexUrl=https://example.com/my-index.json
```

The URL must be https. The file can be a plain `index.json` like the one in this folder, or a
signed `mod-index.v1.txt`. A plain file is read without a signature check and is not cached. The
entries are checked by the same rules either way. The built-in address takes only the signed file.
