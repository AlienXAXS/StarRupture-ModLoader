# StarRupture game-update pipeline

Two workflows that watch Steam for a new StarRupture build and check that the loader and plugins still compile, one result per component. Everything runs on GitHub-hosted runners with the built-in `GITHUB_TOKEN`. No secrets, no self-hosted runner, no Steam login.

## Files (drop into the ModLoader repo)

```
.github/workflows/sr-watch.yml        Steam watcher
.github/workflows/sr-build-all.yml    compile check
.github/sr-pipeline/poll-steam.mjs    the polling script sr-watch runs
.github/sr-pipeline/README.md         this file
```

The `sr-` prefix keeps them clear of `release.yml`, `build-plugins.yml` and the other existing workflows. To install, copy `.github/` into the root of `StarRupture-ModLoader` on `main` (scheduled workflows only run from the default branch).

## What runs when

| Workflow | Trigger | What it does |
|---|---|---|
| `sr-watch.yml` | cron at :07 and :37 every hour; manual (`force` input) | Polls api.steamcmd.net for app 1631270 (branches `public`, `public_test_branch`) and app 3809400 (every non-password branch), plus the newest Steam news title. Retries 3 times per app, then skips that app for the run (warning annotation, no issue). On a real change: opens (or reuses, matched by title) an open `game-update` issue with a per-component checklist, records the ids on the `ci-state` branch (`state/steam.json`), then calls `sr-build-all.yml` with the issue number. |
| `sr-build-all.yml` | nightly 03:23 UTC; manual (branch per repo, default `main`; optional `issue`); called by `sr-watch.yml`; pull requests that change `sr-*.yml` or `.github/sr-pipeline/` | Jobs: `interface-check` (ubuntu; Plugin SDK `PLUGIN_INTERFACE_VERSION` against the loader's `[MIN, MAX]`), `loader` (windows; Client Release and Server Release), `plugin` (windows; BetterDrone, BetterCheats, WAILA, Codex), `report`. Fail-fast is off, so one failing component never hides another. `report` writes the run summary and, when an issue number was given, posts a table on the `game-update` issue: one row per component (loader Client, loader Server, BetterDrone, BetterCheats, WAILA, Codex, plus the interface check) with status, job link and artifact link, and a static "Game SDK: manual" row. Artifacts hold our build outputs only (loader DLLs, plugin DLLs), 3 days retention. |

### Update path per component

The issue body lists what each component needs after a game update. Release lines are always manual, need the owner's OK, and are versioned independently from each repo's own `version` file.

| Component | Today | Later phase hooks in at |
|---|---|---|
| Loader (Client, Server) | compile check; placeholder step "Pattern scan" in the `loader` job | that step (the AOB scan tool) |
| BetterDrone, BetterCheats, WAILA, Codex | rebuild against the current SDK (the `plugin` job) | a sibling job that `needs: plugin` (Claude draft PR on failure) |
| Game SDK | "SDK dump needed (manual)": needs a running game, never automatic | n/a |

One job per component kind (`interface`, `loader`, `plugin`) is the seam: a new phase adds a step to the kind's job or a job after it, and `report` picks up the extra rows without any other restructuring.

### Build arguments

Copied from the loader's `release.yml` and the plugins' `release.yml` build steps (the same arguments `tools/sr-update.ps1` uses), minus everything that tags, releases or uploads: `Configuration`, `Platform=x64`, `PostBuildEventUseInBuild=false`, `ModLoaderBuildTag`, `/m`, `CL_MP=true`, `/nodeReuse:false`. Plugins add `GameSDKRoot` and `PluginSDKInclude` and take their tag from the plugin's `version` file (`dev` if there is none). The loader adds `AutoUpdateManifestUrl` per variant like `release.yml` and a `ci-<run number>` build tag; it builds against the Game SDK commit its submodule pins, so `game_sdk_branch` only affects plugins. Repos come from `vars.SR_OWNER` if that repository variable is set, else from the owner of the repo running the workflow.

Branch inputs default to `main`. To check a feature branch, run `sr-build-all.yml` manually with its name.

What this proves: the tree compiles against the SDKs as they are now and the interface version matches. It does not prove the mods work after a patch: AOB patterns and hard offsets are only exercised in game.

## Permissions

Each workflow starts from `permissions: {}`; every job asks for what it needs.

| Job | Permissions | Why |
|---|---|---|
| `sr-watch` / `poll` | `contents: write`, `issues: write` | push the `ci-state` branch only; create the label and issue |
| `sr-watch` / `build-all` (caller) | `contents: read`, `issues: write`, `actions: read` | must cover the called workflow's jobs, which can only narrow it |
| `sr-build-all` / `interface`, `loader`, `plugin` | `contents: read` | read-only checkouts, `persist-credentials: false` |
| `sr-build-all` / `report` | `contents: read`, `actions: read`, `issues: write` | list this run's jobs and artifacts, comment on the issue |

For pull requests from forks the token is read-only: the build still runs and the `report` job only prints the table in the run summary (the comment step is skipped with a warning, not a failure; PR runs have no issue number anyway).

## Setup after the files land

Nothing needs a secret.

1. Push the files to `main` of the ModLoader repo (write access needed there). Actions must be enabled for the repo; it is, since `release.yml` runs.
2. The `game-update` label is created by the first run that needs it (`gh label create --force`).
3. First run: start `sr-watch` manually. With no `ci-state` branch or no `state/steam.json` it records a baseline and opens no issue.
4. **AlienX sets once, only if applicable:** if the repo's Actions policy is "allow select actions", `actions/checkout@v7.0.1`, `actions/upload-artifact@v7.0.2` and `microsoft/setup-msbuild@v3` must be allowed (he already uses older tags of the same three).

Plugin repos and both SDKs must be public; they are checked out by URL, read-only. Nothing is ever written to them.

## Testing (end to end, without waiting for a real update)

Needs write access to the repo and the `gh` CLI. Replace `OWNER/REPO` with the repo that holds the workflows.

1. Seed the baseline: `gh workflow run sr-watch.yml -R OWNER/REPO`. Wait for it. Expect: green run, summary says "Seeded baseline", no issue, and a `ci-state` branch with `state/steam.json`.
2. Pretend the game was older: put an old build id in the state.

   ```
   git clone --branch ci-state https://github.com/OWNER/REPO.git ci-state-test
   cd ci-state-test
   sed -i 's/"buildid": "[0-9]*"/"buildid": "1"/' state/steam.json
   git commit -am "Test: older build id" && git push origin ci-state
   ```

   (`timeupdated` stays as recorded, so the real current build, which is newer, is not mistaken for a stale API answer.)
3. Run the watcher again: `gh workflow run sr-watch.yml -R OWNER/REPO`.
4. Expect, in this order:
   - a new open issue labelled `game-update`, titled `Game update: client public ..., client public_test_branch ..., server public ...`, whose body has the changed-build table, the newest Steam news title, and the per-component checklist with every line filled in (loader Client and Server, BetterDrone, BetterCheats, WAILA, Codex, Game SDK, each with its release line);
   - a `sr-build-all` run started from the watcher's `build-all` job, with six component jobs plus `interface-check` and `report`;
   - when that run finishes, a comment on the issue: PASS/FAIL, one row per component (status, job link, artifact link), the Game SDK "manual" row, and the branch list.
5. `gh workflow run sr-watch.yml -f force=true -R OWNER/REPO` opens (or reuses, same title) the issue with the recorded ids, without touching the state.
6. Trying the build alone: `gh workflow run sr-build-all.yml -R OWNER/REPO` (add `-f issue=<number>` to get the comment).
7. Clean up: close the test issue; the `ci-state` branch is the real state and now holds the true ids.

## Where GITHUB_TOKEN falls short

- It cannot act on other repositories. So issues and comments land only on the repo running the workflow, and nothing here dispatches or edits a plugin repo. Opening fix PRs on the plugin repos later would need a fine-grained PAT or a GitHub App (an owner-level secret that AlienX would have to create).
- Events caused by `GITHUB_TOKEN` do not start other workflows. The watcher therefore calls `sr-build-all.yml` with `workflow_call` instead of dispatching it, and the state push to `ci-state` triggers nothing.
- A scheduled workflow is switched off after 60 days without repository activity. A push to `ci-state` is expected to count, and the watcher writes one at least every 30 days even when nothing changed. That has not been verified. Check once at about day 50 that the schedule still runs (Actions tab, `SR Steam watch`); if it ever stops, re-enable it there.
- Scheduled runs use the workflow files on the default branch only, so changes to the cron files need to land on `main`.
- Persistent API trouble is a log warning, not a red run. If `ci-state`'s last commit is old and a recent run shows "SKIPPED (API failure)" in its summary, the data source is down.

## What these workflows cannot fire

Checked against the workflow triggers on each repo's default branch (2026-10-29):

- ModLoader `release.yml`: `workflow_dispatch` only. It publishes a GitHub release and uploads to Nexus. Nothing here dispatches it, calls it, tags, edits `version`, or pushes to a branch it reads.
- ModLoader `draft.yml`: runs on any push to a branch named `draft` and creates a release. These workflows never create or push `draft`; the only branch they write is `ci-state`.
- ModLoader `build-plugins.yml` (`workflow_dispatch`, `workflow_call`, uses `PLUGIN_BUILD_PAT` to dispatch plugin releases), `cleanup_tags.yml`, `prune_releases.yml`: not called, not referenced.
- ModLoader `sync-plugin-interface.yml`, `sync-preload-interface.yml`: run on pushes to `main` that change the interface headers and commit to the SDK repos. These workflows push nothing to `main`, and nothing here edits those header paths. Copying the files in is the only push to `main` and it does not touch them.
- Plugin `release.yml` (all four): `workflow_dispatch` only; not triggered. Plugin SDK `build.yml`: runs on pushes to its `main`; we never push there. `plugin-release.yml` is `workflow_call` and is not called.
- No tag is created or pushed, no `gh release`, no Nexus action, no manifest.
