# M3b leg 1: README view logic (detail)

STATUS: done. Reader fixes applied, ReadmeFetcher written and wired, tests green, Core Client Release builds and links.
Branch: `feat/mod-browser-readme` (head: see `git ls-remote` for fork and origin; pushed plain, no version bump, no release).
Knowledge check: none needed (hand-written parser, no outside library).

## Raw results
- Test project (Client Release): `test-build-exit=0`, run: `229 checks, 0 failed`, `run-exit=0` (build\tests\readme_tests.exe).
- StarRupture-ImGui Client Release: `imgui-build-exit=0`.
- StarRupture-ModLoader-Core Client Release: `core-build-exit=0` (readme_fetcher.cpp and readme_markdown.cpp compile with no warnings; the first Core link failed only because StarRupture-ImGui.lib was not built yet).

## Reader fixes (readme_markdown.cpp)
Entity `;` search bounded to 10 bytes; local `min` renamed `lowest`; inline loop stops when spans are full (flushes plain text first); `? 2 : 2` is `2`; blank lines in indented code stop at kMaxCodeBytes.

## API for the UI leg
Headers: `auto_update/readme_fetcher.h` (includes readme_markdown.h, readme_images.h).

```
ModIndex::ReadmeFetcher& ModIndex::ReadmeInstance();          // loader's instance
bool Open(entryId, readmeUrl);   // false: bad address (state Failed) or after Shutdown
void Close();                    // Idle; drops document and bytes; in-flight work discarded
std::shared_ptr<const ReadmeSnapshot> Get() const;           // never null
std::string TakeImageBytes(size_t slot);                     // moves bytes out once
size_t BytesHeld() const;
bool Shutdown(unsigned waitMs);  // wired in core/shutdown.cpp (client build), 2000 ms
```
Snapshot: `state` (Idle/Loading/Ready/Failed), `entryId`, `document` (Ready), `images` (Ready; index = `Block::imageSlot`), `error` (Failed).
Image slot: `url`, `state` (Pending/Loading/Bytes/Failed), `size`, `taken`, `error`.
Block with `kind == Image`: `imageSlot >= 0` means a slot; `-1` means draw `alt` as text (refused address, ninth picture).

Render-thread safe: all of Open, Close, Get, TakeImageBytes, BytesHeld (short lock, no network, no file I/O). Get returns an immutable snapshot, valid while held.

Byte handoff: when `images[i].state == Bytes && !taken`, call `TakeImageBytes(i)` once (on the game thread, where the decode and texture creation run), decode PNG/JPEG/GIF/WebP/BMP, keep the texture, drop the bytes. A second call returns empty. Close()/Open() free any bytes not yet taken. Re-Open refetches everything.

Link spans: `Span::clickable` is true only for https on nexusmods.com or github.com (IsAllowedPageUrl); other links draw as text.

## Limits
| What | Limit |
|---|---|
| README fetch | 512 KB (over: Failed), parser then cuts at 128 KB (`shortened`) |
| Blocks | 1500 (`shortened`) |
| Spans per block | 64 |
| Span bytes | 2000 (longer text continues in next span) |
| Code block | 4000 bytes |
| URL | 200 bytes (link becomes text; image skipped) |
| Pictures | 8 slots (deduped by address), 2 MB each, 6 MB together |
| Picture formats | PNG, JPEG, GIF, WebP, BMP by signature; SVG/XML/HTML refused |
| Picture hosts | raw.githubusercontent.com, user-images.githubusercontent.com, github.com (/o/r/raw/..., /user-attachments/...); never .svg/.svgz |

Caveats: tab becomes 4 spaces; italics lose their markers only; list depth is 0 or 1; `..` clamps at the repo/ref root; images load one after another.

## Not done
- ReadmeTests is not added to StarRupture-ModLoader.sln (it is a manual project; the other test projects are listed there, add if wanted).
- No filters entry: the Core project's .filters file has no mod_index entries, so none were added.
- No UI in this leg.
