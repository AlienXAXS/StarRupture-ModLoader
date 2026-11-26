# Mod index

`index.json` is the list of ModLoader plugins the overlay's Browse tab shows. The loader
downloads it once, when the tab is first opened. The list players get is a signed copy of this file.

## Add a plugin

Add one object to the array and open a pull request. For the whole path from a GitHub release to a
card with an Install button, and on to Nexus, follow [GETTING-LISTED.md](GETTING-LISTED.md).

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
  "interface_version_min": 77,
  "download_url": "https://github.com/AlienXAXS/StarRupture-Plugin-Codex/releases/download/v1.2.0/Codex-Client.dll",
  "download_sha256": "ed04640a6abd9ea0e69ad133056e7e0986260743d5b3b4a753814ffa91286f3b",
  "download_size": 4026880,
  "install_files": ["Codex.dll", "Codex.json"],
  "update_manifest_url": "https://github.com/AlienXAXS/StarRupture-Plugin-Codex/releases/latest/download/Codex-client-manifest.json"
}
```

| Field | Needed | Notes |
|---|---|---|
| `id` | yes | Short, lowercase. Up to 64 characters. |
| `name` | yes | The name shown in the list, and the name the loader shows for the loaded plugin. Up to 64. |
| `page_url` | yes | Where the Browse tab sends the player. See the rule below. Up to 200 characters. |
| `author` | no | Up to 64 characters. |
| `version` | no | The latest release. Up to 32 characters. |
| `summary` | no | One or two sentences. Up to 300 characters. The search box looks in it, so use the words players would type. |
| `readme_url` | no | A raw GitHub link to your README (host `raw.githubusercontent.com`, https). Up to 200 characters. Please include it: the Browse card then shows a README button. See below. |
| `tested_loader` | no | The ModLoader version you tested on, for example `1.27.0`. Up to 32 characters. Please include it. |
| `interface_version_min` | no | The plugin interface number the plugin declares. Leave it out when unknown. |
| `requires` | no | The ids of other plugins this one needs, as a list of text, for example `["minimap"]`. The Browse tab warns a player who disables or uninstalls a plugin yours needs, and Install refuses your plugin until the ones it needs are installed and enabled. The card doesn't list requirements, so also say it in `summary`. |
| `server_only` | no | `true` for a plugin that runs only on a dedicated server. The card then shows **(Server)** after the author, and never has an Install button, even if the install fields are there. Leave it out for any other plugin. See below. |
| `download_url` | for Install | The `.dll` asset of a GitHub release of the repository `page_url` names (so `page_url` is the GitHub repository): `https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.dll`. Up to 200 characters. A name ending `-Server.dll` is not installed. |
| `download_sha256` | for Install | The SHA-256 of that file: 64 lowercase hex characters. |
| `download_size` | for Install | The size of that file in bytes: a whole number from 1 to 33554432 (32 MB). |
| `install_files` | for Install | The name the DLL gets in `ModLoader\Plugins`, and optionally the same name ending `.json`, as a list: `["Codex.dll", "Codex.json"]`. A plain name of letters, digits, `_`, `.` and `-`, up to 80 characters, ending `.dll`. It can differ from the asset's name (`Codex-Client.dll` is installed as `Codex.dll`). |
| `update_manifest_url` | with the `.json` | Needed exactly when `install_files` lists the `.json`, and only then: `https://github.com/<owner>/<repo>/releases/latest/download/<name>-client-manifest.json`, in the same repository. The loader writes the `.json` beside the DLL itself, so the auto-updater keeps the plugin current. |

The list holds up to 200 plugins, and the file stays under 256 KB.

## Install fields

An entry with the install fields (`download_url`, `download_sha256`, `download_size`, `install_files` and,
when the `.json` is listed, `update_manifest_url`) gets an **Install** button on its card. Give all of them
or none. When one is missing or wrong, the loader still lists the entry, with **Open page** and no Install.

What Install does: with an `update_manifest_url`, it reads that release manifest first. It takes the
manifest's `download_url` when the manifest's `version` is higher than the entry's `version` and its
interface range, if it gives both ends, fits the loader. Otherwise (no `update_manifest_url`, a manifest it
can't read, or nothing newer that fits) it takes the entry's `download_url` and checks the size and the
SHA-256 against the entry. Only a file that passes is written, as `install_files`, to
`ModLoader\Plugins`. Install never replaces a file that is already there.

Rules for a newer release:

- The manifest has `version` (up to 32 letters, digits, `.`, `-`, `_` and `+`, such
  as `1.2.0`) and `download_url`.
- `download_url` is a `.dll` release asset of the same repository with the same file name (letter case
  aside) as the entry's `download_url`. If a newer release that isn't skipped for its interface range names
  anything else, Install refuses it and installs nothing. If the entry's `version` has no number to compare, any other address is refused.
- The index has no size or hash for it: the file is at most 32 MB, must start `MZ`, and must match the
  manifest's `download_sha256` (64 hex characters, optional) when there is one.
- A manifest over 1 MB, or one behind a redirect the rules below don't allow, installs nothing. A manifest
  that can't be fetched (no answer, or an error such as 404) installs the entry's file.
- Once the newer release is chosen, a download that fails is a failed install. The entry's file is not
  installed instead.

Rules for the download:

- `https://github.com/<owner>/<repo>/releases/download/...` only, and the owner and repository must be the
  ones in `page_url`. The loader follows GitHub's own redirect to its release download hosts
  (`release-assets.githubusercontent.com` and `objects.githubusercontent.com`) and nothing else. For the
  manifest it first follows GitHub's step from `releases/latest/download/<name>` to
  `releases/download/<tag>/<name>` in the same repository.
- A file attached to a GitHub release. Not another host, not a link to a branch, not a zip of the source.
- Never replace a file after it is listed. A new version is a new release with its own file. To list it,
  change the entry's `version`, `download_url`, `download_sha256` and `download_size` together. With an
  `update_manifest_url` that is optional: the listed file stays the oldest one a player gets and the
  fallback. If the file behind a listed address changes, Install refuses that file.

To get the SHA-256 and the size on Windows, with the file in the current folder:

```
certutil -hashfile Codex-Client.dll SHA256
```

or in PowerShell:

```
(Get-FileHash .\Codex-Client.dll -Algorithm SHA256).Hash.ToLower()
(Get-Item .\Codex-Client.dll).Length
```

The entry needs the hash in lowercase; `certutil` already prints it that way.

## server_only

A plugin that loads only on a dedicated server is no use on a player's PC. Mark it `"server_only": true` and
leave out the install fields. The Browse card shows **(Server)** after the author (the tooltip says "Runs on
a dedicated server") and an **Open page** button, never **Install**: the loader ignores the install fields of
a `server_only` entry, and the check below refuses an entry that has both. The field is optional and append-only: leave it out, or use
`false`, for every other plugin. Older loaders ignore it.

```json
{
  "id": "keepticking",
  "name": "KeepTicking",
  "author": "AlienXAXS",
  "summary": "Dedicated server plugin: keeps the server producing while no players are online.",
  "page_url": "https://github.com/AlienXAXS/StarRupture-Plugin-KeepTicking",
  "server_only": true
}
```

## Search

The Browse tab's search box matches the words a player types against the entry's `name`, `author` and
`summary`, in any case and in any order. A word of 3 letters or more also matches a name that has those
letters in that order, so "minmap" finds "MiniMap". Put the words players would use in the `summary`: what
the plugin does, in plain words.

## The README button

When an entry has a `readme_url`, its card in the Browse tab shows a **README** button that opens your
README inside the window. Use the raw link, for example
`https://raw.githubusercontent.com/<owner>/<repo>/main/README.md`. A picture written with a relative path
is loaded from the same folder as that file. Pictures must be
on GitHub (PNG, JPEG, GIF, WebP or BMP, not SVG), up to 8 per README and 2 MB each. Links to GitHub,
Nexus Mods and YouTube open in the browser. Tables show as plain text.

## Check it before you send it

Save `index.json` with LF line endings and a final newline, then run:

```
node .github/vmap/build-mod-index.mjs mod-index/index.json --out mod-index.txt
```

It prints `ok`, or names the entry that breaks a rule below. It checks the install fields too: a
`download_url` that isn't a release asset of your repository, a `download_sha256` that isn't 64 lowercase
hex characters, a size out of range or not written as plain digits (`2e6` is refused), `install_files` that
don't name exactly one plugin `.dll`, an install field that appears twice or is nested inside another
value, a `server_only` that isn't `true` or `false`, or a `server_only` entry that has install fields.

When you open a PR, a second check also downloads the release file of each new or changed entry. It checks
that the size and SHA-256 match the entry and that the file is a 64-bit plugin DLL with the three plugin
exports. It only reads the file; it never runs it.

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
entries are checked by the same rules either way. A plain file shows the line
"This list isn't signed. It comes from the address you set yourself. Nothing can be installed from it."
and its Install buttons are refused: Install works only from a signed list. The built-in address takes
only the signed file.
