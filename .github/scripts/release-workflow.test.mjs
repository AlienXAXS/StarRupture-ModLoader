// Guards for the resume path of release.yml, read from the workflow text.
//
//   node --test .github/scripts/release-workflow.test.mjs
//
// A resume (resume_tag set) must finish a public release and nothing else: it
// never bumps a version, tags, creates, deletes or replaces anything, and Nexus
// only runs after the release assets were checked.

import test from 'node:test';
import assert from 'node:assert/strict';
import { createPrivateKey, generateKeyPairSync, sign } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { chmodSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { MANIFEST_ASSET } from './release-check.mjs';

const workflows = join(dirname(fileURLToPath(import.meta.url)), '..', 'workflows');
const text = readFileSync(join(workflows, 'release.yml'), 'utf8').replace(/\r\n/g, '\n');

// { jobName: text of that job } from the top-level `jobs:` block.
const jobs = Object.fromEntries(
  [...text.slice(text.indexOf('\njobs:\n')).matchAll(/\n  ([\w-]+):\n([\s\S]*?)(?=\n  [\w-]+:\n|\n*$)/g)].map((m) => [m[1], m[2]]),
);
const ifOf = (job) => /\n    if: (\|\n(?: {6}.*\n?)+|.*)/.exec(jobs[job])?.[1] ?? '';

test('the jobs are the ones this test knows', () => {
  assert.deepEqual(Object.keys(jobs), ['prepare', 'build', 'release', 'verify-release', 'nexus', 'publish-feed', 'notice', 'sync-plugin-sdk', 'trigger-plugin-builds']);
});

test('a failed Nexus step puts the manual fallback in the run, and the last job names the resume command', () => {
  assert.match(jobs.nexus, /name: Say how to publish Nexus by hand\n {8}if: \$\{\{ failure\(\) \}\}[\s\S]*resume-notice\.mjs nexus-text[\s\S]*::error title=Nexus publish failed::/);
  assert.match(ifOf('notice'), /always\(\) && needs\.prepare\.result == 'success'/);
  assert.match(jobs.notice, /needs: \[prepare, release, verify-release, nexus, publish-feed\]/);
  assert.match(jobs.notice, /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" >/);
  assert.match(jobs.notice, /resume-notice\.mjs release/);
  assert.doesNotMatch(text, /issues: write|gh issue/);
});

test('only the build and the release job are skipped on a resume', () => {
  assert.match(ifOf('build'), /resume_tag == ''/);
  assert.match(ifOf('release'), /resume_tag == ''/);
  for (const job of ['verify-release', 'publish-feed']) {
    assert.doesNotMatch(ifOf(job), /resume_tag == ''/, `${job} must run on a resume`);
  }
  // Nexus names resume_tag only to apply resume_nexus on a resume; a fresh run always runs it.
  assert.doesNotMatch(ifOf('nexus'), /&&\s*github\.event\.inputs\.resume_tag == ''/);
});

test('a resume computes no version and finds its release first', () => {
  const prepare = jobs.prepare;
  assert.match(prepare, /name: Generate release tag\n {8}id: tag\n {8}if: \$\{\{ github.event.inputs.resume_tag == '' \}\}/);
  assert.match(prepare, /name: Find the release to resume\n {8}id: resume\n {8}if: \$\{\{ github.event.inputs.resume_tag != '' \}\}/);
  assert.match(prepare, /releases\/tags\/\$RESUME_TAG/);
  assert.match(prepare, /nothing to resume/);
});

test('a tag that already exists stops a new run instead of bumping or reusing it', () => {
  assert.match(jobs.prepare, /git ls-remote --exit-code --tags origin "refs\/tags\/\$tag"[\s\S]*resume_tag=\$tag/);
});

test('jobs that run on a resume never tag, create, replace, delete or force', () => {
  const forbidden = /gh release (create|delete|edit|upload)|git (tag|push)|--clobber|--force|--cleanup-tag|gh api -X (DELETE|PATCH|PUT|POST)/;
  for (const job of ['prepare', 'verify-release', 'nexus']) {
    assert.doesNotMatch(jobs[job], forbidden, `${job} must not change the release`);
  }
});

test('Nexus waits for the asset check, which does not ask for the version map', () => {
  assert.match(jobs.nexus, /needs: \[prepare, release, verify-release\]/);
  assert.match(ifOf('nexus'), /needs\.verify-release\.result == 'success'/);
  assert.match(jobs['verify-release'], /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" --version-map skip/);
  assert.match(jobs.nexus, /gh release download \$env:TAG/, 'Nexus takes the files that were checked');
  assert.doesNotMatch(text, /modloader-zips/);
});

test('the resume options reach Nexus', () => {
  assert.match(jobs.nexus, /name: Upload Client build to Nexus Mods\n {8}if: \$\{\{ github.event.inputs.resume_tag == '' \|\| github.event.inputs.resume_nexus != 'server-only' \}\}/);
  assert.match(ifOf('nexus'), /\(github\.event\.inputs\.resume_tag == '' \|\| github\.event\.inputs\.resume_nexus != 'none'\)/, 'a fresh run ignores resume_nexus');
});

test('a pwsh step must not end on a native exit code that is not a failure', () => {
  // The runner ends a pwsh step with $LASTEXITCODE: ls-remote exits 2 for "no such tag"
  // and git show exits 128 for "no such file", both normal here.
  assert.match(jobs.prepare, /git ls-remote --exit-code --tags origin "refs\/tags\/\$tag" \*> \$null\n.*\n.*\n.*\n {10}\$global:LASTEXITCODE = 0\n/);
  assert.match(jobs.nexus, /git show "refs\/tags\/[^\n]*2>\$null[^\n]*\n.*\n {10}\$global:LASTEXITCODE = 0\n/);
});

test('the manifest the release uploads has the name the check reads', () => {
  assert.ok(jobs.release.includes(`"$ws\\${MANIFEST_ASSET}"`));
  assert.match(jobs.release, /release-check\.mjs" write-manifest --tag \$tag --out \$manifest @assets\n.*\n {10}\$assets\.Add\(\$manifest\)/);
});

test('one release run at a time', () => {
  assert.match(text, /\nconcurrency:\n {2}group: release\n {2}cancel-in-progress: false\n/);
});

// The release carries the signed version object map from the moment it is public.
const vmapStep = /- name: Fetch the signed version map for the release\n((?: {8}.*\n|\n)+?)(?= {6}- name:)/.exec(`${jobs.release}\n`)[1];
const vmapPublish = readFileSync(join(workflows, 'vmap-publish.yml'), 'utf8').replace(/\r\n/g, '\n');

test('the release step takes the version map from the same place, with the same check, as vmap-publish', () => {
  for (const [name, value] of [['FEED_REPO', 'msantoro12/starrupture-ci'], ['FEED_RELEASE', 'vmap-feed'], ['FEED_ASSET', 'version-map-client.v1.txt']]) {
    assert.ok(vmapStep.includes(`${name}: ${value}\n`), name);
  }
  // The two defaults in vmap-publish.yml are the same strings.
  assert.ok(vmapPublish.includes("default: 'msantoro12/starrupture-ci'") && vmapPublish.includes("default: 'vmap-feed'") && vmapPublish.includes('FEED_ASSET: version-map-client.v1.txt'));
  // The same verifier, key source and test-key refusal; no second verifier.
  assert.match(vmapStep, /node \.github\/vmap\/verify-feed\.mjs "\$FEED_ASSET" --source StarRupture-ModLoader-Core\/memory_scanner\/version_feed\.cpp --refuse-test-key/);
  assert.ok(vmapPublish.includes('node .github/vmap/verify-feed.mjs "$RUNNER_TEMP/new/$FEED_ASSET" "${args[@]}"') && vmapPublish.includes('--source StarRupture-ModLoader-Core/memory_scanner/version_feed.cpp'));
});

test('a missing or unverifiable version map does not stop the release, and the file goes on it when there is one', () => {
  assert.match(vmapStep, /\n {8}continue-on-error: true\n/);
  assert.match(vmapStep, /^ {8}if: \$\{\{ needs\.prepare\.outputs\.is_preview != 'true' \}\}\n/);
  // It runs before the release is created, and the create step adds the file to the upload list after the manifest.
  assert.ok(jobs.release.indexOf('Fetch the signed version map for the release') < jobs.release.indexOf('name: Create GitHub Release'));
  assert.match(jobs.release, /\$assets\.Add\(\$manifest\)\n[\s\S]*\$versionMap = "\$ws\\version-map-client\.v1\.txt"\n {10}if \(Test-Path \$versionMap\) \{ \$assets\.Add\(\$versionMap\) \}\n[\s\S]*gh release create \$tag @assets @flags/);
  // The version map is not in the manifest, so a newer copy from vmap-publish.yml is not a size or md5 mismatch.
  assert.ok(jobs.release.indexOf('write-manifest') < jobs.release.indexOf('$assets.Add($versionMap)'));
});

// The step, run for real with a fake `gh`.
function runVmapStep({ feed, missing = false, refuse = false }) {
  const dir = mkdtempSync(join(tmpdir(), 'rel-vmap-'));
  const ws = join(dir, 'ws');
  const bin = join(dir, 'bin');
  mkdirSync(join(ws, '.github', 'vmap'), { recursive: true });
  mkdirSync(join(ws, '.github', 'scripts'), { recursive: true });
  mkdirSync(join(ws, 'StarRupture-ModLoader-Core', 'memory_scanner'), { recursive: true });
  mkdirSync(bin);
  const here = dirname(fileURLToPath(import.meta.url));
  copyFileSync(join(here, '..', 'vmap', 'verify-feed.mjs'), join(ws, '.github', 'vmap', 'verify-feed.mjs'));
  copyFileSync(join(here, 'resume-notice.mjs'), join(ws, '.github', 'scripts', 'resume-notice.mjs'));
  writeFileSync(join(ws, 'StarRupture-ModLoader-Core', 'memory_scanner', 'version_feed.cpp'),
    `constexpr const char* kK1Hex = "${feed.test.hex}";\nconstexpr bool kK1TestOnly = true;\nconstexpr const char* kK2Hex = "${feed.release.hex}";\nconstexpr bool kK2TestOnly = false;\n`);
  writeFileSync(join(dir, 'feed.txt'), feed.file);
  writeFileSync(join(bin, 'gh'), `#!/bin/bash
echo "$*" >> "${posix(join(dir, 'calls.log'))}"
${missing || refuse ? 'exit 1' : `dir=.; while [ $# -gt 0 ]; do [ "$1" = --dir ] && dir="$2"; shift; done; cp "${posix(join(dir, 'feed.txt'))}" "$dir/version-map-client.v1.txt"`}
`);
  chmodSync(join(bin, 'gh'), 0o755);
  const step = vmapStep.replace(/^ {8}[\s\S]*?\n {8}run: \|\n/, '').replace(/^ {10}/gm, '');
  const r = spawnSync('bash', ['-c', step], {
    cwd: ws,
    encoding: 'utf8',
    windowsHide: true,
    env: {
      ...process.env,
      PATH: `${posix(bin)}${delimiter}${process.env.PATH}`,
      FEED_REPO: 'x/ci', FEED_RELEASE: 'vmap-feed', FEED_ASSET: 'version-map-client.v1.txt', TAG: 'v1.2.3',
      GITHUB_REPOSITORY: 'o/r', GITHUB_REF_NAME: 'main', GH_TOKEN: 'x',
    },
  });
  return { ...r, placed: existsSync(join(ws, 'version-map-client.v1.txt')) };
}
const posix = (p) => p.replace(/\\/g, '/');

const keyPair = () => {
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  return { pem: privateKey.export({ type: 'pkcs8', format: 'pem' }), hex: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32).toString('hex') };
};
const KEYS = { test: keyPair(), release: keyPair() };
const signFeed = (k, id, body) => Buffer.from(`${body}sig\t${id}\t${sign(null, Buffer.from(body), createPrivateKey(k.pem)).toString('base64')}\n`);
const SEQ = Math.floor(Date.now() / 1000) - 1000;
const GOOD_BODY = `format\t1\nseq\t${SEQ}\nbuild\t128786\t0x22222222\t0x2000000\tpublic_test_branch 2\nfield\tATest::Reflected\t128786\t0x30\t0x8\t0x0\t-\n`;
const withKeys = (file, extra = {}) => ({ file, ...KEYS, ...extra });
const RESUME = 'gh workflow run release.yml --repo o/r --ref main -f resume_tag=v1.2.3 -f resume_nexus=none';

test('a version map that verifies is put in the workspace for the release', () => {
  const r = runVmapStep({ feed: withKeys(signFeed(KEYS.release, 'k2', GOOD_BODY)) });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(r.placed, true);
  assert.doesNotMatch(r.stdout, /::error/);
});

test('a version map that is missing or cannot be downloaded fails the step with the resume command, and leaves no file', () => {
  const r = runVmapStep({ feed: withKeys(signFeed(KEYS.release, 'k2', GOOD_BODY)), missing: true });
  assert.equal(r.status, 1);
  assert.equal(r.placed, false);
  assert.match(r.stdout, /::error title=No version map on the release::The signed version map could not be downloaded from release vmap-feed in x\/ci\. The release goes out without it\./);
  assert.ok(r.stdout.includes(`run: ${RESUME}`), r.stdout);
});

test('a version map that does not verify fails the step with the resume command, and leaves no file', () => {
  const bad = {
    'a tampered file': Buffer.from(signFeed(KEYS.release, 'k2', GOOD_BODY).toString('latin1').replace('0x30', '0x31'), 'latin1'),
    'a duplicate build': signFeed(KEYS.release, 'k2', `${GOOD_BODY}build\t128786\t0x22222222\t0x2000000\tx\n`),
    'a TEST-key file': signFeed(KEYS.test, 'k1', GOOD_BODY),
  };
  for (const [what, file] of Object.entries(bad)) {
    const r = runVmapStep({ feed: withKeys(file) });
    assert.equal(r.status, 1, what);
    assert.equal(r.placed, false, what);
    assert.match(r.stdout, /::error title=No version map on the release::The signed version map from x\/ci does not verify/, what);
    assert.ok(r.stdout.includes(`run: ${RESUME}`), what);
  }
});

// The Client build must carry the version map URL and the release key, or the release fails before upload.
const dllStep = /- name: Check the Client build carries the version map URL and key\n((?: {8}.*\n|\n)+?)(?= {6}- name:)/.exec(`${jobs.build}\n`)[1];
const KEY_SOURCE = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'StarRupture-ModLoader-Core', 'memory_scanner', 'version_feed.cpp');
const realKeys = (src) => ({ k1: /kK1Hex = "([0-9a-f]{64})"/.exec(src)[1], k2: /kK2Hex = "([0-9a-f]{64})"/.exec(src)[1] });

test('the DLL check runs for the Client build only, after the build and before the upload', () => {
  assert.match(dllStep, /^ {8}if: \$\{\{ matrix\.variant == 'Client' \}\}\n/);
  const at = (s) => jobs.build.indexOf(s);
  assert.ok(at('name: Build Client Release') < at('Check the Client build carries') && at('Check the Client build carries') < at('name: Upload ${{ matrix.variant }} build'));
  // The URL it looks for is the one the build is given.
  assert.ok(jobs.build.includes('/p:VmapFeedUrl="https://github.com/${{ github.repository }}/releases/latest/download/version-map-client.v1.txt"'));
  assert.ok(dllStep.includes('url="https://github.com/$GITHUB_REPOSITORY/releases/latest/download/version-map-client.v1.txt"'));
});

function runDllStep({ dll, keySource = KEY_SOURCE, repo = 'o/r' }) {
  const dir = mkdtempSync(join(tmpdir(), 'dll-'));
  const path = join(dir, 'Core.dll');
  if (dll !== null) writeFileSync(path, dll);
  const step = dllStep.replace(/^ {8}[\s\S]*?\n {8}run: \|\n/, '').replace(/^ {10}/gm, '');
  return spawnSync('bash', ['-c', step], {
    encoding: 'utf8',
    windowsHide: true,
    env: { ...process.env, DLL: posix(path), KEY_SOURCE: posix(keySource), GITHUB_REPOSITORY: repo },
  });
}
// A stand-in for a DLL: binary noise with the given strings in it, NUL-terminated as the compiler stores them.
const dllWith = (...strings) => Buffer.concat([Buffer.alloc(4096, 0x90), ...strings.flatMap((s) => [Buffer.from(s), Buffer.alloc(1), Buffer.alloc(64, 0x41)])]);
const URL_FOR = (repo) => `https://github.com/${repo}/releases/latest/download/version-map-client.v1.txt`;

test('a Core DLL with the version map URL and the release key passes', () => {
  const keys = realKeys(readFileSync(KEY_SOURCE, 'utf8'));
  const r = runDllStep({ dll: dllWith(URL_FOR('o/r'), keys.k2, keys.k1) });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /carries the version map URL and 1 release public key/);
  // The TEST key (slot k1) is not needed: a build may leave it out.
  assert.equal(runDllStep({ dll: dllWith(URL_FOR('o/r'), keys.k2) }).status, 0);
});

test('a Core DLL without the URL, with another repository\'s URL, without the release key, or missing fails before upload', () => {
  const keys = realKeys(readFileSync(KEY_SOURCE, 'utf8'));
  const noUrl = runDllStep({ dll: dllWith(keys.k2, keys.k1) });
  assert.equal(noUrl.status, 1);
  assert.match(noUrl.stdout, /::error title=Client build is missing the version map setting::The Core DLL does not contain the version map URL https:\/\/github\.com\/o\/r\/releases\/latest\/download\/version-map-client\.v1\.txt \(VmapFeedUrl\)\. Nothing was uploaded and no release was made\./);
  assert.equal(runDllStep({ dll: dllWith(URL_FOR('someone/else'), keys.k2) }).status, 1);
  const noKey = runDllStep({ dll: dllWith(URL_FOR('o/r'), keys.k1) });
  assert.equal(noKey.status, 1);
  assert.match(noKey.stdout, /does not contain the release public key of slot k2/);
  const gone = runDllStep({ dll: null });
  assert.equal(gone.status, 1);
  assert.match(gone.stdout, /The built Core DLL was not found/);
});

test('a key source with no release key fails the check instead of passing it', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ks-'));
  const src = join(dir, 'version_feed.cpp');
  writeFileSync(src, 'constexpr const char* kK1Hex = "' + 'a'.repeat(64) + '";\nconstexpr bool kK1TestOnly = true;\nconstexpr const char* kK2Hex = "";\nconstexpr bool kK2TestOnly = false;\n');
  const r = runDllStep({ dll: dllWith(URL_FOR('o/r'), 'a'.repeat(64)), keySource: src });
  assert.equal(r.status, 1);
  assert.match(r.stdout, /names no release public key/);
});
