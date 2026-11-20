// Runs the prune step of prune_releases.yml for real with a fake `gh`.
//
//   node --test .github/scripts/prune-releases.test.mjs
//
// The release before Latest is kept with Latest, so a broken Latest can be rolled
// back to. A draft is never deleted (a release run may be using it), nor is its
// tag. Nothing is deleted on a dry run.

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const text = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'workflows', 'prune_releases.yml'), 'utf8').replace(/\r\n/g, '\n');
const posix = (p) => p.replace(/\\/g, '/');
const STEP = /\n {8}run: \|\n((?: {10}.*\n|\n)+)/.exec(text)[1].replace(/^ {10}/gm, '');

// Newest first, as the API lists them: id, tag, draft, prerelease.
const RELEASES = [
  [105, 'v1.27.0-rc.1', false, true],
  [104, 'v1.26.0', false, false], // Latest
  [103, 'v1.25.1', true, false],  // a draft: never the rollback target, never deleted
  [102, 'v1.25.0-beta', false, true],
  [101, 'v1.25.0', false, false], // the release before Latest
  [100, 'v1.24.0', false, false],
];
const TAGS = ['v0.9.0', 'v1.24.0', 'v1.25.0', 'v1.25.0-beta', 'v1.26.0', 'v1.27.0-rc.1', 'v1.25.1'];

const FAKE = String.raw`#!/bin/bash
echo "$*" >> "$FAKE_DIR/calls.log"
case "$*" in
  *"-X DELETE"*) exit 0;;
  *releases/latest*) cat "$FAKE_DIR/latest"; exit 0;;
  *"/tags "*) cat "$FAKE_DIR/tags"; exit 0;;
  *"/releases "*) cat "$FAKE_DIR/releases"; exit 0;;
esac
echo "fake gh: unexpected call: $*" >&2
exit 9
`;

function prune({ dryRun, releases = RELEASES, latest = 'v1.26.0', tags = TAGS }) {
  const dir = mkdtempSync(join(tmpdir(), 'prune-'));
  const bin = join(dir, 'bin');
  const work = join(dir, 'work');
  mkdirSync(bin);
  mkdirSync(work);
  writeFileSync(join(bin, 'gh'), FAKE);
  chmodSync(join(bin, 'gh'), 0o755);
  writeFileSync(join(dir, 'latest'), `${latest}\n`);
  writeFileSync(join(dir, 'tags'), tags.map((t) => `${t}\n`).join(''));
  writeFileSync(join(dir, 'releases'), releases.map((r) => `${r.join('\t')}\n`).join(''));
  writeFileSync(join(dir, 'calls.log'), '');
  const r = spawnSync('bash', ['-c', STEP], {
    cwd: work,
    encoding: 'utf8',
    windowsHide: true,
    env: { ...process.env, PATH: `${posix(bin)}${delimiter}${process.env.PATH}`, FAKE_DIR: posix(dir), GH_TOKEN: 'x', REPO: 'o/r', DRY_RUN: String(dryRun) },
  });
  const deletes = readFileSync(join(dir, 'calls.log'), 'utf8').split('\n').filter((l) => l.includes('-X DELETE'));
  return { ...r, deletes };
}

test('a dry run lists the previous release and the draft as kept, and deletes nothing', () => {
  const r = prune({ dryRun: true });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /^Keeping latest release: v1\.26\.0$/m);
  assert.match(r.stdout, /^Keeping the previous release: v1\.25\.0$/m);
  const toDelete = /Releases to delete \(\d+\):\n((?:  .*\n)+)/.exec(r.stdout)[1];
  assert.deepEqual(toDelete.trim().split('\n').map((l) => l.trim()), ['v1.27.0-rc.1 [ prerelease ]', 'v1.25.0-beta [ prerelease ]', 'v1.24.0']);
  assert.match(r.stdout, /^Keeping 1 draft release\(s\), left to the release run or to the owner: v1\.25\.1$/m);
  const tagList = /Tags to delete \(\d+\):\n((?:  .*\n)+)/.exec(r.stdout)[1].trim().split('\n').map((l) => l.trim());
  assert.ok(!tagList.includes('v1.25.0') && !tagList.includes('v1.26.0'), 'both kept tags stay');
  assert.ok(!tagList.includes('v1.25.1'), 'the tag a draft will get stays');
  assert.deepEqual(r.deletes, []);
});

test('a real run deletes neither Latest, the previous release nor a draft, nor their tags', () => {
  const r = prune({ dryRun: false });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  const gone = r.deletes.join('\n');
  assert.doesNotMatch(gone, /releases\/(104|103|101)\b/);
  assert.doesNotMatch(gone, /refs\/tags\/v1\.(26\.0|25\.0|25\.1)$/m);
  for (const id of [105, 102, 100]) assert.match(gone, new RegExp(`releases/${id}$`, 'm'));
  assert.match(gone, /refs\/tags\/v0\.9\.0$/m);
});

test('a draft.yml branch build (tag ending -draft) is pruned as before, with its tag; a release draft still is not', () => {
  const releases = [[107, 'v1.25.2-draft', true, true], [106, 'v1.25.1', true, false], ...RELEASES.filter((r) => r[1] !== 'v1.25.1')];
  const r = prune({ dryRun: false, releases, tags: [...TAGS, 'v1.25.2-draft'] });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  const gone = r.deletes.join('\n');
  assert.match(gone, /releases\/107$/m);
  assert.match(gone, /refs\/tags\/v1\.25\.2-draft$/m);
  assert.doesNotMatch(gone, /releases\/106$/m);
  assert.doesNotMatch(gone, /refs\/tags\/v1\.25\.1$/m);
  assert.match(r.stdout, /^Keeping 1 draft release\(s\), left to the release run or to the owner: v1\.25\.1$/m);
});

test('with no release before Latest only Latest is kept, and no tag is lost to an empty pattern', () => {
  const r = prune({ dryRun: false, releases: [[104, 'v1.26.0', false, false], [105, 'v1.27.0-rc.1', false, true]], tags: ['v1.26.0', 'v0.9.0'] });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /There is no release before the latest one to keep\./);
  const gone = r.deletes.join('\n');
  assert.doesNotMatch(gone, /releases\/104$/m);
  assert.doesNotMatch(gone, /refs\/tags\/v1\.26\.0$/m);
  assert.match(gone, /refs\/tags\/v0\.9\.0$/m);
});

test('no Latest release still refuses to delete anything', () => {
  const r = prune({ dryRun: false, latest: '' });
  assert.equal(r.status, 1);
  assert.deepEqual(r.deletes, []);
});
