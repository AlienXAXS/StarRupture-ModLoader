# StarRupture compile check

`sr-build-all.yml` checks that the loader and the plugins still compile, one result per component. It runs on GitHub-hosted runners with the built-in `GITHUB_TOKEN`. No secrets, no self-hosted runner.

Steam update alerts do not live in this repository. The Steam watcher, the update pull requests it opens and the per-component result comment on them are in `msantoro12/starrupture-ci`.

## What runs when

| Trigger | What it does |
|---|---|
| nightly 03:23 UTC; manual (a branch per repo, default `main`); pull requests that change `sr-*.yml` or `.github/sr-pipeline/` | `interface-check` (Plugin SDK `PLUGIN_INTERFACE_VERSION` against the loader's `[MIN, MAX]`), `loader` (Client Release and Server Release), `plugin` (BetterDrone, BetterCheats, WAILA, Codex), `report`. Fail-fast is off, so one failing component never hides another. `report` writes the table to the run summary: one row per component with status, job link and artifact link, and a static "Game SDK: manual" row. Artifacts hold build outputs only, 3 days retention. |

This only says whether the code compiles and the plugin interface version matches. It does not say whether the mods work in game.

## Build arguments

Copied from the loader's `release.yml` and the plugins' `release.yml` build steps (the same arguments `tools/sr-update.ps1` uses), minus everything that tags, releases or uploads. The loader builds against the Game SDK commit its submodule pins, so `game_sdk_branch` only affects plugins.

## Permissions

`permissions: {}` at the top; each job asks for what it needs: `contents: read` for the build jobs, plus `actions: read` for `report` (it lists this run's jobs and artifacts).

## Not touched

This workflow does not call or reference `release.yml`, `draft.yml`, `build-plugins.yml`, `cleanup_tags.yml`, `prune_releases.yml` or the `sync-*` workflows, and it never creates or pushes a `draft` branch.
