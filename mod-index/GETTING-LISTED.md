# Getting your mod into Browse, and onto Nexus

The Browse tab in the mod loader window lists plugins from the [mod index](README.md). A listed plugin that
has a download gets an **Install** button: the loader downloads it from your GitHub release, checks it and
loads it, with no file to copy. These steps get your plugin there, and then onto Nexus Mods.

The worked example is a made-up plugin, Quick Haul, by `example-author`.

## 1. Publish a GitHub release

1. Build the plugin and tag a release in your repository, for example `v1.0.0`.
2. Attach the plugin's `.dll` to the release as its own file (a release asset), for example
   `QuickHaul-Client.dll`. Install downloads that file as it is, so a `.zip` does not work.
3. Name it with letters, digits, `_`, `.` and `-` only. A name ending `-Server.dll` is for dedicated
   servers and is not installed on a player's PC (see step 5).
4. Never replace or re-upload that file afterwards. Fix a mistake with a new release.

Only files attached to a GitHub release of your own repository can be the download. The address must start
with `https://github.com/`, and no other site works.

## 2. Work out the SHA-256 and the size

The index lists the SHA-256 and the size of the file, and the loader checks both before it writes anything.
In a Windows command prompt, with the file in the current folder:

```
certutil -hashfile QuickHaul-Client.dll SHA256
dir QuickHaul-Client.dll
```

Or in PowerShell:

```
(Get-FileHash .\QuickHaul-Client.dll -Algorithm SHA256).Hash.ToLower()
(Get-Item .\QuickHaul-Client.dll).Length
```

Use the hash in lowercase, 64 characters, with no spaces. The size is in bytes. Do it on the file you
uploaded: download it from the release page and hash that copy, so you know it is the one players get.

## 3. Add your entry

Add one object to `mod-index/index.json` (the full field list is in [README.md](README.md)):

```json
{
  "id": "quickhaul",
  "name": "QuickHaul",
  "author": "example-author",
  "version": "1.0.0",
  "summary": "Haul items between storage faster: a hotkey moves a full stack to the nearest chest.",
  "page_url": "https://github.com/example-author/StarRupture-Plugin-QuickHaul",
  "readme_url": "https://raw.githubusercontent.com/example-author/StarRupture-Plugin-QuickHaul/main/README.md",
  "tested_loader": "1.28.0",
  "download_url": "https://github.com/example-author/StarRupture-Plugin-QuickHaul/releases/download/v1.0.0/QuickHaul-Client.dll",
  "download_sha256": "9f2c1a7e5b4d3c8a6e0f1b2d4c5a7e9f8b6d3c1a2e4f5a7b9c0d1e2f3a4b5c6d",
  "download_size": 183296,
  "install_files": ["QuickHaul.dll"],
  "thumbnail_url": "https://raw.githubusercontent.com/example-author/StarRupture-Plugin-QuickHaul/3f1c9a0b7d5e4a2c8b6f0d1e9a7c5b3d2f4e6a80/images/thumbnail.png",
  "thumbnail_sha256": "b1946ac92492d2347c6235b4d2611184c3a6f6ad0f1e5b8f9d3c2a7e4b5d6c01",
  "thumbnail_size": 48213
}
```

What Browse shows for each field:

| Field | On the card |
|---|---|
| `name`, `author`, `version` | The heading: name, version, "by" author. |
| `summary` | The line under the heading, and the words the search box looks in. |
| `page_url` | The **Open page** button. With a download, it is your GitHub repository (the same one as `download_url`). Without one, a github.com or nexusmods.com page works. |
| `readme_url` | A **README** button that opens your README in the window. |
| `tested_loader` | "Tested on 1.28.0". |
| `download_url`, `download_sha256`, `download_size`, `install_files` | The **Install** button. Without any one of them the card has no Install button, only **Open page**. |
| `thumbnail_url`, `thumbnail_sha256`, `thumbnail_size` | The picture on the card. Without all three the card has no picture. See "Add a thumbnail" below. |
| `requires` | Install is refused until the mods you name are installed and enabled: "QuickHaul needs MiniMap. Install MiniMap first." Add `"requires": ["minimap"]` (their `id` values) and say it in `summary` too. |

`install_files` is the name the DLL gets in the player's `ModLoader\Plugins` folder. It can differ from the
asset name. Install never replaces a file: if `QuickHaul.dll` is already there, it stops and says so.

Auto-updates are optional. If your release also carries a `<Name>-client-manifest.json`, list the `.json` as
well and add the manifest address, and the loader writes the small `.json` beside the DLL so the
auto-updater keeps the plugin current:

```json
  "install_files": ["QuickHaul.dll", "QuickHaul.json"],
  "update_manifest_url": "https://github.com/example-author/StarRupture-Plugin-QuickHaul/releases/latest/download/QuickHaul-client-manifest.json"
```

The `.json` and `update_manifest_url` go together: both or neither.

### Add a thumbnail (optional)

A card can show a picture of your plugin.

1. Make a PNG and commit it to your repository as `images/thumbnail.png`. Use 16:9: 640 x 360 is a
   good size. The loader accepts only PNG files, up to 1280 x 720 and at most 524288 bytes. A picture of
   another shape is shown whole, smaller than its frame.
2. Copy the full 40-character id of the commit that holds it (not a branch name like `main`), and build the
   address `https://raw.githubusercontent.com/<owner>/<repo>/<commit>/images/thumbnail.png`. The owner and
   repository are the ones in `page_url`.
3. Work out the SHA-256 and the size as in step 2:

   ```
   certutil -hashfile images\thumbnail.png SHA256
   dir images\thumbnail.png
   ```

4. Add the three fields to your entry, as in the example above: `thumbnail_url`, `thumbnail_sha256` and
   `thumbnail_size`. Give all three or none.

Never change the file behind a listed address. A new picture is a new commit, so a new address, a new
SHA-256 and a new size, changed together. An entry whose `page_url` is a Nexus Mods page takes no thumbnail
fields. To match your Nexus page you can use the same artwork as your Nexus main image, but not the same
file: Nexus serves its main image as WebP (even when the address ends in `.png`), often larger than
1280 x 720, and the loader reads PNG only. Export your own 16:9 PNG.

## 4. Write the name, author and summary so people find it

The search box matches what a player types against your `name`, `author` and `summary`, in any case and in
any order. A short word also matches a name that has those letters in order ("minmap" finds MiniMap), but
the summary matches wherever your word appears in it. So:

- Put the words players would type in the `summary`, in plain words: what it does, what it shows, what it
  changes. Players type "map", "minimap", "recipe", "drone", "cheats", "server", "hotkey", "build".
- Name what the mod is, not a joke. "QuickHaul" is found by "haul" and "quick"; add "storage", "chest" and
  "hotkey" in the summary and it is also found by those.
- Keep `author` the name you use on GitHub or Nexus. Players search it.

## 5. Server-only mods

A plugin that runs only on a dedicated server is no use on a player's PC. Add `"server_only": true` and
leave out the download fields:

```json
  "server_only": true
```

Its card shows **(Server)** after the author, with the tooltip "Runs on a dedicated server", and an **Open
page** button. It never has an Install button: the loader ignores download fields on a `server_only`
entry, and the check in step 6 refuses an entry that has both.

## 6. Check it and send it

Save `index.json` with LF line endings and a final newline, and run the check:

```
node .github/vmap/build-mod-index.mjs mod-index/index.json --out mod-index.txt
```

It prints `ok`, or names the entry and the rule it breaks. It also refuses a download size written as
`2e6`, an install field written twice, install fields on a `server_only` entry, and thumbnail fields that
are not all three, are wrong, or sit on a Nexus Mods entry. Then open a PR with the new entry, the way
[README.md](README.md) describes. To try the Install button before it is merged, point your own copy of the
loader at your file with `ModIndexUrl` (see "Try your own index" in the same README).

## 7. Nexus Mods

1. Publish the GitHub release first (step 1), and get it into the index. Update the Nexus page or file only
   for a version that is already a GitHub release.
2. Then upload the same version to Nexus. Use the same version number and the same images on both, so the
   Browse card, the GitHub release and the Nexus page match.
3. Keep `page_url` on your GitHub repository. An entry with a download needs `page_url` and
   `download_url` to name the same repository, so **Open page** goes to GitHub, and your Nexus page address
   goes in your README and release notes.

## When you release again

A new version is a new release with its own file. Change `version`, `download_url`, `download_sha256` and
`download_size` together in your entry, and keep the old release's file where it is. If the file behind a
listed address changes, Install refuses it.
