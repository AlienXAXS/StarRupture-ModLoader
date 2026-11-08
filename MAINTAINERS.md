# Maintainer notes: the version-map feed

What a loader maintainer needs to know about the signed feed that refreshes the
[version map](VersionMap.md) between loader releases. The map's contents and how plugins use it are
in `VersionMap.md`. This page covers only the loader side. For the CI side, see
[the workflow chain](https://github.com/msantoro12/starrupture-ci/blob/main/ci/MAP-MODE.md#the-workflow-chain)
(from sr-watch through vmap-publish) and
[publishing and the watchdog](https://github.com/msantoro12/starrupture-ci/blob/main/ci/MAP-MODE.md#publishing-and-the-watchdog)
(vmap-publish, first_publish, why the feed sits on Latest).

## Format

A tab-separated text file, one row per line, in the order `format`, `seq`, `build`, `function` /
`absent`, `field`, then a last line `sig<TAB><keyId><TAB><base64>`.

- `format` is `1`. The loader reads only the format it was built for.
- `seq` is the UTC second of signing. It is a plain decimal number, never 0 and with no leading zero.
- `sig` is an Ed25519 signature (RFC 8032) by the key in slot `<keyId>` over every byte before the
  `sig` line, `seq` line included.

## Asset and URL

The asset is `version-map-client.v1.txt` on the loader's Latest release. The name carries the format
version, so a later format never strands a loader that reads this one. There is no server feed; only
the client fetches.

The URL is the compiled define `VMAP_FEED_URL` (`/p:VmapFeedUrl=` in `Shared.props`) and is never read
from `modloader.ini`. `draft.yml` and `release.yml` set it to
`https://github.com/<repository>/releases/latest/download/version-map-client.v1.txt`. A `static_assert`
in `auto_updater.cpp` requires a non-empty URL to end in `/version-map-client.v<format>.txt`. An empty
define, or no compiled public key, means no fetch.

## When the loader fetches

Only on the client, with `[AutoUpdate] Enabled` on (`feed_fetch.h`, `feed_fetch.cpp`):

- **Stage 1**, at startup, with the game's main thread parked (where WinHTTP is safe), when the
  running exe is in neither the built-in map nor the cache. It also runs when the exe is known but
  has an `absent` row that a request can bind this launch (a global key, a plugin's site key, or a
  `ModLoader/` key under the running loader's fingerprint), the cache has not filled it and the cache
  file is over a day old. The splash shows "Downloading game data for CL-n".
- **After a refusal**, when the version check or the pattern preflight refuses the build and Stage 1
  did not just ask: a fetch for the next launch, before the process exits.
- **Stage 3**, from the auto-update thread, at most once a day, for the next launch.

An exe the map has never seen asks at every launch until a cached feed covers it. The once-a-day
limit applies to the `absent` case and to Stage 3. The cache file's timestamp is touched on every
request, so "over a day old" counts from the last request and not from the last change.

## Request

One HTTPS GET (`http_get.cpp`). A URL that is not `https` is refused, redirects from HTTPS to HTTP
are disallowed, each WinHTTP operation has a 2 s timeout (`kOperationMs`), the whole request has an
8 s deadline (`kDeadlineMs`), and the body is capped at 1 MiB while it is read (`kMaxFileBytes`).

## Cache

The signed file itself, `ModLoader\version-map-client.v1.cache`, written to a temp file and then
renamed over. It is verified again on every launch and is only replaced by a file with a higher
`seq`, so an old signed file cannot be replayed over a newer one.

## Signature checking

The file is verified before anything in it is read (`version_feed.cpp`, `ReadSigned`). It is rejected
whole if it is over 1 MiB, has a malformed line, has a `sig` line that is not last, names a key the
loader does not hold, has a `sig` that is not 64 bytes of base64, or fails the signature check.
Verification uses vendored Monocypher (`third_party/monocypher`, Ed25519 with SHA-512), which has no
handles and behaves the same on Windows and Wine.

## Key slots

Two public-key slots are compiled in (`version_feed.cpp`), so a key can be rotated by a loader release
that adds the new key to the free slot and a later one that drops the old.

| Slot | Key | Trusted by |
|---|---|---|
| `k1` | TEST key | only a build that defines `VMAP_TRUST_TEST_KEY` (the unit tests and `sr-aobscan`) |
| `k2` | release key | every build |

A release build therefore trusts only `k2`, and a feed signed with `k1` is rejected by it. The private
halves are CI secrets held in the CI repository (`VMAP_SIGN_KEY_TEST`, `VMAP_SIGN_KEY`); this
repository holds none of them. A leaked key is answered by a loader release that drops its slot. A
slot with an empty key holds nothing; with both empty the loader reads and fetches no feed.

## Merge

The loader merges the built-in map and the feed into one row per key and CL (per `fp` for a site
key), once at startup, and the result stays unchanged for the process. For each group it takes the
built-in row for the running exe if there is one, otherwise the feed's, otherwise the built-in. The
feed can add builds and keys and fill a shipped `absent` row; it can never change a locator the
loader shipped with for the running exe. A CL known only from the feed is supported only if the union
holds a locator for every required loader entry and loader field on it; otherwise it is dropped and
the build stays refused.

## What a bad feed can do

It cannot run code. Every row still has to pass its kind check, the uniqueness rule, equality with its
`rva` on its own exe, and for a native field the class-size bound. The worst a wrong signed row can do
is point a plugin's hook at the wrong game function, which is why the file is signed.

## Publishing

`.github/workflows/vmap-publish.yml` is a pull workflow and holds no signing key. It downloads the
signed feed from the `vmap-feed` release of the feed repository (inputs `feed_repo`, `feed_release`),
checks it with `.github/vmap/verify-feed.mjs` against the public keys in `version_feed.cpp`, and
uploads it to this repository's Latest release.

- It runs every 15 minutes and on demand. A new release gets its feed at the next run, because
  `release.yml` does not attach the file and a release made with `GITHUB_TOKEN` starts no `release:
  published` workflow. Between releases the file is replaced whenever the `seq` rises.
- A feed that fails verification fails the run and is not uploaded. A valid feed whose `seq` does not
  rise above the newest one on any release is not uploaded either, except that a Latest with no feed
  of its own takes the current one even at an equal `seq`. It is never given an older one.
- The `seq` to beat is that of the newest release that carries a feed, read only after that file
  verifies.
- With no feed on any release, only a manual run with `first_publish` set uploads one, so an older
  signed feed cannot be the first.
- The scheduled run refuses a feed signed with the TEST key; only a manual run with `allow_test_key`
  set accepts one, for tests on a fork.
- Latest is where the asset sits because `prune_releases.yml` deletes every other release and tag.
- Release builds carry version-map rows for every game build the pipeline has mapped.
