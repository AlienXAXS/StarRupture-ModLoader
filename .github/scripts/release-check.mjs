#!/usr/bin/env node
// Checks that a release is complete. release.yml runs it on the draft before it
// is published, before the Nexus upload and again at the end of the run; anything
// else that needs to know whether a release is whole (a "covered" test, a
// person) calls the same file.
//
//   node release-check.mjs --repo <owner/name> --tag <tag>
//        [--pre-publish] [--version-map auto|require|skip] [--cl <n>]
//   node release-check.mjs find --repo <owner/name> --tag <tag>
//
// What it checks, with only `gh` and this one file (it imports nothing else):
//   - the release for the tag is published (a tag alone, or a draft, is not).
//     With --pre-publish it is the draft for the tag instead: GitHub answers a
//     draft's tag with 404, so the draft is found in the releases list;
//   - every asset listed in the release's own `release-assets.json` is there,
//     fully uploaded, with the listed size and md5. release.yml writes that file
//     from the same list it uploads, so the expected set is never kept twice;
//   - the signed version object map (`version-map-client.v1.txt`, the asset
//     release.yml attaches to the draft) when the release is meant to carry one.
//     `auto` (the default) means the release is not a preview and is the repo's
//     Latest release (with --pre-publish: is not a preview, because publishing
//     makes it Latest); `require` always; `skip` never (the check after the
//     Nexus upload uses it). The file is checked for shape only (format line,
//     seq, signature line); the signature itself is checked by verify-feed.mjs
//     before it is attached.
//   - --cl <n>: the version map has a build row for that game build. Implies
//     `require` unless `skip` is given.
//
// Output: one JSON object on stdout. Exit 0 complete, 1 incomplete (see
// "problems"), 2 the check itself could not run (gh failed, bad arguments).
// A release made before release-assets.json existed reports `manifest_missing`.
//
// `find` says what exists for the tag, without downloading anything:
// {"state":"public"|"draft"|"none"|"ambiguous", "id", "prerelease", "target"}.
// It exits 0 whatever the state is.
//
// `write-manifest --tag <tag> --out <file> <asset>...` writes that file; see
// manifestFor. release.yml calls it just before it creates the release.

import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';

export const MANIFEST_ASSET = 'release-assets.json';
export const VERSION_MAP_ASSET = 'version-map-client.v1.txt';
export const EXIT_COMPLETE = 0;
export const EXIT_INCOMPLETE = 1;
export const EXIT_ERROR = 2;
const VERSION_MAP_CAP = 1024 * 1024;

const md5 = (buf) => createHash('md5').update(buf).digest('hex');

// io: { release(repo, tag) -> release | null, drafts(repo, tag) -> [release],
//       latestTag(repo) -> string | null, download(repo, asset) -> Buffer }.
// `release` is the GitHub release object (id, draft, prerelease, target_commitish,
// assets[{ id, name, size, state }]).
export function checkRelease({ repo, tag, versionMap = 'auto', cl = null, prePublish = false }, io) {
  const problems = [];
  const out = { repo, tag, complete: false, release: { exists: false }, expected: 0, problems };

  let rel;
  if (prePublish) {
    const drafts = io.drafts(repo, tag);
    if (drafts.length > 1) problems.push({ code: 'draft_ambiguous', count: drafts.length });
    else if (drafts.length === 0) problems.push({ code: 'draft_missing' });
    rel = drafts.length === 1 ? drafts[0] : null;
  } else {
    rel = io.release(repo, tag);
    if (!rel) problems.push({ code: 'release_missing' });
  }
  if (!rel) return out;
  const byName = new Map(rel.assets.map((a) => [a.name, a]));
  out.release = { exists: true, draft: prePublish, prerelease: !!rel.prerelease, latest: null };

  // The list the release says it should hold.
  const mAsset = byName.get(MANIFEST_ASSET);
  let expected = [];
  if (!mAsset) {
    problems.push({ code: 'manifest_missing', name: MANIFEST_ASSET });
  } else {
    try {
      const m = JSON.parse(io.download(repo, mAsset).toString('utf8'));
      if (m.tag !== tag) throw new Error(`lists tag ${m.tag}`);
      if (!Array.isArray(m.assets) || m.assets.some((a) => !a.name || !Number.isInteger(a.size) || !/^[0-9a-f]{32}$/.test(a.md5))) {
        throw new Error('an entry has no name, size or md5');
      }
      expected = m.assets;
    } catch (e) {
      problems.push({ code: 'manifest_invalid', name: MANIFEST_ASSET, detail: e.message });
    }
  }
  out.expected = expected.length;

  for (const want of expected) {
    const have = byName.get(want.name);
    if (!have) { problems.push({ code: 'asset_missing', name: want.name }); continue; }
    if (have.state !== 'uploaded') { problems.push({ code: 'asset_not_uploaded', name: want.name, state: have.state }); continue; }
    if (have.size !== want.size) { problems.push({ code: 'size_mismatch', name: want.name, expected: want.size, actual: have.size }); continue; }
    const actual = md5(io.download(repo, have));
    if (actual !== want.md5) problems.push({ code: 'md5_mismatch', name: want.name, expected: want.md5, actual });
  }

  if (versionMap !== 'skip') {
    let need = versionMap === 'require' || cl !== null;
    if (!need && !rel.prerelease) {
      // A draft is never Latest yet; publishing it makes it Latest.
      out.release.latest = prePublish ? false : io.latestTag(repo) === tag;
      need = prePublish || out.release.latest;
    }
    if (need) checkVersionMap(byName.get(VERSION_MAP_ASSET), cl, repo, io, problems, out);
  }

  out.complete = problems.length === 0;
  return out;
}

function checkVersionMap(asset, cl, repo, io, problems, out) {
  const name = VERSION_MAP_ASSET;
  if (!asset) { problems.push({ code: 'version_map_missing', name }); return; }
  if (asset.state !== 'uploaded') { problems.push({ code: 'asset_not_uploaded', name, state: asset.state }); return; }
  if (asset.size === 0 || asset.size > VERSION_MAP_CAP) { problems.push({ code: 'version_map_invalid', name, detail: `size ${asset.size}` }); return; }
  const lines = io.download(repo, asset).toString('utf8').split('\n').filter((l) => l !== '');
  const seq = /^seq\t(\d+)$/.exec(lines[1] ?? '');
  if (lines[0] !== 'format\t1' || !seq || !/^sig\t/.test(lines[lines.length - 1])) {
    problems.push({ code: 'version_map_invalid', name, detail: 'not a signed format 1 file' });
    return;
  }
  out.versionMap = { seq: Number(seq[1]) };
  if (cl !== null && !lines.some((l) => l.startsWith(`build\t${cl}\t`))) {
    problems.push({ code: 'version_map_cl_missing', name, cl });
  }
}

// The real gh calls. A 404 from the API is an answer; any other failure is not.
export function ghIo(run = (args) => spawnSync('gh', args, { maxBuffer: 1 << 30, windowsHide: true })) {
  const call = (args) => {
    const r = run(args);
    if (r.error) throw new Error(`gh could not run: ${r.error.message}`);
    if (r.status === 0) return r.stdout;
    const err = String(r.stderr);
    if (/HTTP 404|Not Found/.test(err)) return null;
    throw new Error(`gh ${args.join(' ')} failed: ${err.trim()}`);
  };
  return {
    release: (repo, tag) => {
      const body = call(['api', `repos/${repo}/releases/tags/${tag}`]);
      return body && JSON.parse(String(body));
    },
    // releases/tags/{tag} never answers for a draft; the list does, to a token with
    // push access. One compact JSON object per line.
    drafts: (repo, tag) => {
      const body = call(['api', '--paginate', `repos/${repo}/releases`, '--jq', '.[] | select(.draft == true) | tojson']);
      if (!body) return [];
      return String(body).split('\n').filter(Boolean).map((l) => JSON.parse(l)).filter((r) => r.tag_name === tag);
    },
    latestTag: (repo) => {
      const body = call(['api', `repos/${repo}/releases/latest`]);
      return body ? JSON.parse(String(body)).tag_name : null;
    },
    download: (repo, asset) => {
      const body = call(['api', '-H', 'Accept: application/octet-stream', `repos/${repo}/releases/assets/${asset.id}`]);
      if (!body) throw new Error(`asset ${asset.name} could not be downloaded`);
      return body;
    },
  };
}

// The file release.yml uploads with the release: name, size and md5 of every
// other asset, taken from the files themselves just before they are uploaded.
export function manifestFor(tag, files) {
  return {
    tag,
    assets: files.map((f) => {
      const body = readFileSync(f);
      return { name: basename(f), size: body.length, md5: md5(body) };
    }),
  };
}

//   node release-check.mjs write-manifest --tag <tag> --out <file> <asset>...
function writeManifest(argv) {
  const [tagFlag, tag, outFlag, out, ...files] = argv;
  if (tagFlag !== '--tag' || !tag || outFlag !== '--out' || !out || files.length === 0) {
    throw new Error('usage: write-manifest --tag <tag> --out <file> <asset>...');
  }
  writeFileSync(out, `${JSON.stringify(manifestFor(tag, files), null, 2)}\n`);
  return files.length;
}

// What exists for the tag: a public release, a draft (found in the list), or nothing.
export function findRelease({ repo, tag }, io) {
  const describe = (state, r) => ({ repo, tag, state, id: r.id, prerelease: !!r.prerelease, target: r.target_commitish });
  const published = io.release(repo, tag);
  if (published) return describe('public', published);
  const drafts = io.drafts(repo, tag);
  if (drafts.length === 0) return { repo, tag, state: 'none' };
  if (drafts.length > 1) return { repo, tag, state: 'ambiguous', count: drafts.length };
  return describe('draft', drafts[0]);
}

function parseArgs(argv) {
  const opts = { versionMap: 'auto', cl: null, prePublish: false };
  for (let i = 0; i < argv.length; i += 2) {
    if (argv[i] === '--pre-publish') {
      opts.prePublish = true;
      i -= 1;   // a flag with no value
      continue;
    }
    const [flag, value] = [argv[i], argv[i + 1]];
    if (value === undefined) throw new Error(`${flag} needs a value`);
    if (flag === '--repo') opts.repo = value;
    else if (flag === '--tag') opts.tag = value;
    else if (flag === '--version-map') opts.versionMap = value;
    else if (flag === '--cl') opts.cl = value;
    else throw new Error(`unknown argument ${flag}`);
  }
  if (!/^[\w.-]+\/[\w.-]+$/.test(opts.repo ?? '')) throw new Error('--repo <owner/name> is required');
  if (!/^[\w.+-]+$/.test(opts.tag ?? '')) throw new Error('--tag <tag> is required');
  if (!['auto', 'require', 'skip'].includes(opts.versionMap)) throw new Error('--version-map is auto, require or skip');
  if (opts.cl !== null && !/^\d+$/.test(opts.cl)) throw new Error('--cl is a number');
  return opts;
}

export function main(argv, io = ghIo()) {
  let result;
  try {
    if (argv[0] === 'write-manifest') {
      console.log(JSON.stringify({ manifest: writeManifest(argv.slice(1)) }));
      return EXIT_COMPLETE;
    }
    if (argv[0] === 'find') {
      console.log(JSON.stringify(findRelease(parseArgs(argv.slice(1)), io)));
      return EXIT_COMPLETE;
    }
    result = checkRelease(parseArgs(argv), io);
  } catch (e) {
    console.log(JSON.stringify({ complete: false, error: e.message }));
    return EXIT_ERROR;
  }
  console.log(JSON.stringify(result));
  return result.complete ? EXIT_COMPLETE : EXIT_INCOMPLETE;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exitCode = main(process.argv.slice(2));
}
