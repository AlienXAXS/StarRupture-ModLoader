// Guards for release.yml, read from the workflow text.
//
//   node --test .github/scripts/release-workflow.test.mjs
//
// A release is made as a draft, checked while nothing is public and published by
// one job; everything that reads or builds on it comes after that job. A resume
// (resume_tag set) of a public release must finish it and nothing else: it never
// bumps a version, tags, creates, deletes or replaces anything, and Nexus only
// runs after the release assets were checked.

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
  assert.deepEqual(Object.keys(jobs), ['prepare', 'build', 'release', 'inspect', 'verify-release', 'publish', 'nexus', 'publish-feed', 'notice', 'sync-plugin-sdk', 'trigger-plugin-builds']);
});

test('the release is a draft until the publish job, which comes after the check', () => {
  // Created as a draft on a fixed commit; no git tag is made by the release job.
  assert.match(jobs.release, /\$flags \+= @\('--draft', '--target', "\$\{\{ needs\.prepare\.outputs\.sha \}\}"\)\n\s+gh release create \$tag @assets @flags/);
  assert.doesNotMatch(jobs.release, /git (tag|push)|--draft=false|gh release edit/);
  // Only the publish job makes a release public, and only after the draft passed the check.
  const publishers = Object.keys(jobs).filter((j) => /--draft=false/.test(jobs[j]));
  assert.deepEqual(publishers, ['publish']);
  assert.match(jobs.publish, /needs: \[prepare, verify-release\]/);
  assert.match(ifOf('publish'), /needs\.verify-release\.result == 'success'/);
  assert.match(jobs['verify-release'], /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" --pre-publish \|/);
  assert.match(jobs['verify-release'], /needs: \[prepare, inspect, release\]/);
  // The publish makes the tag from the commit the draft was given; it does not delete or force anything.
  assert.doesNotMatch(jobs.publish, /--clobber|--force|gh release (create|delete|upload)|gh api -X (DELETE|PATCH|PUT|POST)|git (tag|push)/);
});

test('everything that reads or builds on the release comes after the publish job', () => {
  for (const job of ['nexus', 'publish-feed', 'sync-plugin-sdk', 'trigger-plugin-builds']) {
    assert.match(jobs[job], /(^|\n) {4}needs: \[[^\]]*\bpublish\b[^\]]*\]/, `${job} must need publish`);
    assert.match(ifOf(job), /needs\.publish\.result == 'success'/, `${job} must run only after a successful publish`);
  }
  assert.match(jobs.notice, /needs: \[[^\]]*\bpublish\b/);
});

test('a failed Nexus step puts the manual fallback in the run, and the last job names the resume command', () => {
  assert.match(jobs.nexus, /name: Say how to publish Nexus by hand\n {8}if: \$\{\{ failure\(\) \}\}[\s\S]*resume-notice\.mjs nexus-text[\s\S]*::error title=Nexus publish failed::/);
  assert.match(ifOf('notice'), /always\(\) && needs\.prepare\.result == 'success'/);
  assert.match(jobs.notice, /needs: \[prepare, inspect, release, verify-release, publish, nexus, publish-feed\]/);
  assert.match(jobs.notice, /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" >/);
  assert.match(jobs.notice, /resume-notice\.mjs release/);
  assert.doesNotMatch(text, /issues: write|gh issue/);
});

test('a public release is not built again; a draft is rebuilt only when it is not whole', () => {
  // Fresh run, or a resumed draft that the inspect job found not whole.
  assert.match(ifOf('build'), /needs\.prepare\.outputs\.state == 'new'/);
  assert.match(ifOf('build'), /needs\.prepare\.outputs\.state == 'draft' && needs\.inspect\.outputs\.complete == 'false'/);
  assert.doesNotMatch(ifOf('build'), /'public'/);
  assert.match(ifOf('release'), /needs\.build\.result == 'success'/);
  // Both build from the commit the draft was made on, never from the ref of this run.
  for (const job of ['build', 'release']) {
    assert.match(jobs[job], /uses: actions\/checkout@[^\n]*\n {8}with:\n {10}ref: \$\{\{ needs\.prepare\.outputs\.sha \}\}\n/, `${job} must check out the draft's commit`);
  }
  // The public release is checked by `inspect`, and publish-feed follows it.
  assert.match(ifOf('inspect'), /state == 'public' \|\| needs\.prepare\.outputs\.state == 'draft'/);
  assert.doesNotMatch(ifOf('publish-feed'), /resume_tag == ''/, 'publish-feed must run on a resume');
  assert.match(ifOf('publish-feed'), /needs\.prepare\.outputs\.state == 'public' && needs\.inspect\.result == 'success'/);
  // Nexus names resume_tag only to apply resume_nexus on a resume; a fresh run always runs it.
  assert.doesNotMatch(ifOf('nexus'), /&&\s*github\.event\.inputs\.resume_tag == ''/);
});

test('a resume finds its release by state and never computes a version', () => {
  const prepare = jobs.prepare;
  assert.match(prepare, /name: Generate release tag\n {8}id: tag\n {8}if: \$\{\{ github.event.inputs.resume_tag == '' \}\}/);
  assert.match(prepare, /name: Find the release to resume\n {8}id: resume\n {8}if: \$\{\{ github.event.inputs.resume_tag != '' \}\}/);
  assert.match(prepare, /release-check\.mjs find --repo "\$GITHUB_REPOSITORY" --tag "\$RESUME_TAG"/);
  assert.match(prepare, /nothing to resume/);
  assert.match(prepare, /More than one draft/);
  // A draft whose target is not a commit cannot be rebuilt.
  assert.match(prepare, /\^\[0-9a-f\]\{40\}\$/);
});

test('a draft that is not whole is rebuilt whole: the inspect job decides, the release job replaces every asset', () => {
  // inspect: exit 0 whole, 1 not whole (not an error), anything else stops the run.
  assert.match(jobs.inspect, /complete: \$\{\{ steps\.draft\.outputs\.complete \}\}/);
  assert.match(jobs.inspect, /--pre-publish \| tee[\s\S]*0\) echo "complete=true"[\s\S]*1\) echo "complete=false"[\s\S]*\*\) echo "::error[\s\S]*exit "\$code"/);
  // verify-release also runs for a draft that was whole and skipped the build.
  assert.match(ifOf('verify-release'), /needs\.release\.result == 'success' \|\|\n\s+\(needs\.prepare\.outputs\.state == 'draft' && needs\.inspect\.outputs\.complete == 'true'\)/);
  // The release job: only a draft is touched, every old asset goes, one upload with --clobber.
  const create = /- name: Create GitHub Release\n([\s\S]*?)(?=\n {6}- name:|\n*$)/.exec(jobs.release)[1];
  const repair = /if \(\$env:RELEASE_STATE -eq 'draft'\) \{([\s\S]*?)\n {10}\}\n/.exec(create)[1];
  assert.match(repair, /\.draft\n[\s\S]*-ne 'true'\) \{ throw/, 'it checks the release is still a draft first');
  assert.match(repair, /releases\/\$\(\$env:DRAFT_ID\)\/assets/);
  assert.match(repair, /gh api -X DELETE "repos\/\$repo\/releases\/assets\/\$id"/);
  assert.match(repair, /gh release upload \$tag @assets --clobber --repo \$repo/);
  assert.match(repair, /exit 0$/, 'the repair never falls through to a create');
  assert.ok(create.indexOf('RELEASE_STATE') < create.indexOf('gh release create'));
  // No delete of a release or tag anywhere in the workflow, and no force (the SDK sync
  // pushes to the SDK repo, not this one).
  assert.doesNotMatch(text, /gh release delete|--cleanup-tag|--force/);
  assert.doesNotMatch(text.replace(jobs['sync-plugin-sdk'], ''), /git push|git tag/);
  assert.equal([...text.matchAll(/gh release upload [^\n]*--clobber/g)].length, 1, 'only the draft repair replaces assets');
});

test('the SDK sync and the plugin builds take the tag from prepare and the DLLs from the build only when there was one', () => {
  assert.match(jobs['sync-plugin-sdk'], /needs: \[prepare, release, publish\]/);
  assert.match(jobs['sync-plugin-sdk'], /BUILD_TAG: \$\{\{ needs\.prepare\.outputs\.tag \}\}/);
  assert.match(jobs['sync-plugin-sdk'], /HAVE_ARTIFACT: \$\{\{ needs\.release\.result == 'success' \}\}/);
  // Sync only: the Latest release, not the first entry of a list that can start with a draft.
  assert.match(jobs['sync-plugin-sdk'], /gh api "repos\/\$\{\{ github\.repository \}\}\/releases\/latest" --jq \.tag_name/);
  assert.doesNotMatch(jobs['sync-plugin-sdk'], /gh release list/);
  assert.match(jobs['trigger-plugin-builds'], /needs: \[prepare, publish\]/);
  for (const job of ['sync-plugin-sdk', 'trigger-plugin-builds']) {
    assert.match(ifOf(job), /needs\.prepare\.outputs\.is_preview != 'true'/, `${job} must not run for a preview`);
  }
});

test('a tag that already exists stops a new run instead of bumping or reusing it', () => {
  assert.match(jobs.prepare, /git ls-remote --exit-code --tags origin "refs\/tags\/\$tag"[\s\S]*resume_tag=\$tag/);
});

test('jobs that run on a resume never tag, create, replace, delete or force', () => {
  const forbidden = /gh release (create|delete|edit|upload)|git (tag|push)|--clobber|--force|--cleanup-tag|gh api -X (DELETE|PATCH|PUT|POST)/;
  for (const job of ['prepare', 'inspect', 'verify-release', 'nexus']) {
    assert.doesNotMatch(jobs[job], forbidden, `${job} must not change the release`);
  }
});

test('Nexus waits for a published release that passed the asset check', () => {
  assert.match(jobs.nexus, /needs: \[prepare, inspect, release, publish\]/);
  // Fresh: the publish job ran after the draft check. Resumed public release: the inspect job passed.
  assert.match(ifOf('nexus'), /needs\.publish\.result == 'success' \|\|\n\s+\(needs\.prepare\.outputs\.state == 'public' && needs\.inspect\.result == 'success'\)/);
  assert.match(jobs.inspect, /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" --version-map skip/);
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

test('a missing or unverifiable version map stops the run before a release exists, and the file goes on the draft', () => {
  assert.doesNotMatch(vmapStep, /continue-on-error/);
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
  mkdirSync(join(ws, 'StarRupture-ModLoader-Core', 'memory_scanner'), { recursive: true });
  mkdirSync(bin);
  const here = dirname(fileURLToPath(import.meta.url));
  copyFileSync(join(here, '..', 'vmap', 'verify-feed.mjs'), join(ws, '.github', 'vmap', 'verify-feed.mjs'));
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
      FEED_REPO: 'x/ci', FEED_RELEASE: 'vmap-feed', FEED_ASSET: 'version-map-client.v1.txt',
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

test('a version map that verifies is put in the workspace for the release', () => {
  const r = runVmapStep({ feed: withKeys(signFeed(KEYS.release, 'k2', GOOD_BODY)) });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(r.placed, true);
  assert.doesNotMatch(r.stdout, /::error/);
});

test('a version map that is missing or cannot be downloaded fails the step before any release is made, and leaves no file', () => {
  const r = runVmapStep({ feed: withKeys(signFeed(KEYS.release, 'k2', GOOD_BODY)), missing: true });
  assert.equal(r.status, 1);
  assert.equal(r.placed, false);
  assert.match(r.stdout, /::error title=No version map for the release::The signed version map could not be downloaded from release vmap-feed in x\/ci\. No release was made\. Run this workflow again/);
});

test('a version map that does not verify fails the step before any release is made, and leaves no file', () => {
  const bad = {
    'a tampered file': Buffer.from(signFeed(KEYS.release, 'k2', GOOD_BODY).toString('latin1').replace('0x30', '0x31'), 'latin1'),
    'a duplicate build': signFeed(KEYS.release, 'k2', `${GOOD_BODY}build\t128786\t0x22222222\t0x2000000\tx\n`),
    'a TEST-key file': signFeed(KEYS.test, 'k1', GOOD_BODY),
  };
  for (const [what, file] of Object.entries(bad)) {
    const r = runVmapStep({ feed: withKeys(file) });
    assert.equal(r.status, 1, what);
    assert.equal(r.placed, false, what);
    assert.match(r.stdout, /::error title=No version map for the release::The signed version map from x\/ci does not verify/, what);
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
