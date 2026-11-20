// Runs the steps of vmap-publish.yml's `publish` job for real, in order, with a
// fake `gh` and the real verify-feed.mjs, and checks what the run does.
//
//   node --test .github/vmap/vmap-publish.test.mjs
//
// The steps are read from the workflow text, so a change there is what is tested.
// Keys are generated for the run and written into a copy of the key source.

import test from 'node:test';
import assert from 'node:assert/strict';
import { createPrivateKey, generateKeyPairSync, sign } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { chmodSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const workflow = readFileSync(join(here, '..', 'workflows', 'vmap-publish.yml'), 'utf8').replace(/\r\n/g, '\n');
const posix = (p) => p.replace(/\\/g, '/');

// The steps of the `publish` job that run a script: { name, id, if, run }.
const publishJob = workflow.slice(workflow.indexOf('\n  publish:\n'), workflow.indexOf('\n  notice:\n'));
const STEPS = publishJob.split('\n      - name: ').slice(1).map((block) => {
  const run = /\n {8}run: (?:\|\n((?: {10}.*\n?)+)|(.*)\n?)/.exec(`\n${block}`);
  return {
    name: block.split('\n')[0],
    id: /\n {8}id: (\w+)/.exec(block)?.[1],
    cond: /\n {8}if: (.*)/.exec(block)?.[1],
    run: run ? (run[1] ? run[1].replace(/^ {10}/gm, '') : run[2]) : null,
  };
}).filter((s) => s.run !== null);

const pair = () => {
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  return { pem: privateKey.export({ type: 'pkcs8', format: 'pem' }), hex: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32).toString('hex') };
};
const TEST_KEY = pair();   // slot k1, flagged as a TEST key
const RELEASE_KEY = pair();   // slot k2
const signedWith = (k, id, body) => Buffer.from(`${body}sig\t${id}\t${sign(null, Buffer.from(body), createPrivateKey(k.pem)).toString('base64')}\n`);

const BASE = Math.floor(Date.now() / 1000) - 100000;
const ROW = 'build\t128786\t0x22222222\t0x2000000\tpublic_test_branch 2\nfield\tATest::Reflected\t128786\t0x30\t0x8\t0x0\t-\n';
const feed = (seq, { id = 'k2', body = ROW } = {}) => signedWith(id === 'k1' ? TEST_KEY : RELEASE_KEY, id, `format\t1\nseq\t${seq}\n${body}`);
const DUPLICATE = ROW + 'field\tATest::Reflected\t128786\t0x40\t0x8\t0x0\t-\n';

// The fake `gh`: answers from files in FAKE_DIR and logs every call.
const FAKE = String.raw`#!/bin/bash
echo "$*" >> "$FAKE_DIR/calls.log"
what="$1 $2"
tag="$3"
all="$*"
repo=
dir=
while [ $# -gt 0 ]; do
  case "$1" in
    --repo) repo="$2"; shift;;
    --dir) dir="$2"; shift;;
  esac
  shift
done
case "$what" in
  "release view")
    case "$all" in
      *isPrerelease*) cat "$FAKE_DIR/prerelease"; exit 0;;
      *"--json tagName"*) cat "$FAKE_DIR/latest"; exit 0;;
      *"--json assets"*) cat "$FAKE_DIR/assets"; exit 0;;
    esac;;
  "release list") cat "$FAKE_DIR/releases"; exit 0;;
  "release download")
    if [ "$repo" = "$FEED_REPO" ]; then src="$FAKE_DIR/new.txt"; else src="$FAKE_DIR/rel-$tag.txt"; fi
    [ -f "$src" ] || exit 1
    mkdir -p "$dir"
    cp "$src" "$dir/version-map-client.v1.txt"
    exit 0;;
  "release upload") exit 0;;
esac
echo "fake gh: unexpected call: $all" >&2
exit 9
`;

// Runs the job. `releases` is newest first: [{ tag, feed? }]; the first is Latest.
function run({ releases, newFeed, env = {}, prerelease = false, brokenKeySource = false }) {
  const dir = mkdtempSync(join(tmpdir(), 'vmap-pub-'));
  const fake = join(dir, 'fake');
  const bin = join(dir, 'bin');
  const ws = join(dir, 'ws');
  for (const d of [fake, bin, join(ws, '.github', 'vmap'), join(ws, 'StarRupture-ModLoader-Core', 'memory_scanner'), join(dir, 'tmp')]) mkdirSync(d, { recursive: true });
  writeFileSync(join(bin, 'gh'), FAKE);
  chmodSync(join(bin, 'gh'), 0o755);
  copyFileSync(join(here, 'verify-feed.mjs'), join(ws, '.github', 'vmap', 'verify-feed.mjs'));
  writeFileSync(join(ws, 'StarRupture-ModLoader-Core', 'memory_scanner', 'version_feed.cpp'),
    brokenKeySource ? 'nothing here\n'
      : `constexpr const char* kK1Hex = "${TEST_KEY.hex}";\nconstexpr bool kK1TestOnly = true;\nconstexpr const char* kK2Hex = "${RELEASE_KEY.hex}";\nconstexpr bool kK2TestOnly = false;\n`);
  writeFileSync(join(fake, 'latest'), `${releases[0].tag}\n`);
  writeFileSync(join(fake, 'releases'), releases.map((r) => `${r.tag}\n`).join(''));
  writeFileSync(join(fake, 'prerelease'), `${prerelease}\n`);
  writeFileSync(join(fake, 'assets'), 'version-map-client.v1.txt\n');
  writeFileSync(join(fake, 'calls.log'), '');
  for (const r of releases) if (r.feed) writeFileSync(join(fake, `rel-${r.tag}.txt`), r.feed);
  if (newFeed) writeFileSync(join(fake, 'new.txt'), newFeed);

  const out = join(dir, 'out');
  writeFileSync(out, '');
  const baseEnv = {
    ...process.env,
    PATH: `${posix(bin)}${delimiter}${process.env.PATH}`,
    FAKE_DIR: posix(fake),
    FEED_REPO: 'x/ci',
    FEED_RELEASE: 'vmap-feed',
    ALLOW_TEST_KEY: 'false',
    FIRST_PUBLISH: 'false',
    FEED_ASSET: 'version-map-client.v1.txt',
    GITHUB_REPOSITORY: 'o/r',
    GH_TOKEN: 'x',
    RUNNER_TEMP: posix(join(dir, 'tmp')),
    GITHUB_OUTPUT: posix(out),
    ...env,
  };
  const outputs = {};
  const ran = [];
  let status = 0;
  for (const step of STEPS) {
    const gate = step.cond && /steps\.(\w+)\.outputs\.(\w+) (!=|==) '(.*)'/.exec(step.cond);
    if (gate) {
      const value = outputs[gate[1]]?.[gate[2]] ?? '';
      if (gate[3] === '!=' ? value === gate[4] : value !== gate[4]) continue;
    }
    const script = step.run.replace(/\$\{\{ steps\.(\w+)\.outputs\.(\w+) \}\}/g, (_, id, k) => outputs[id]?.[k] ?? '');
    const r = spawnSync('bash', ['-c', script], { cwd: ws, env: baseEnv, encoding: 'utf8', windowsHide: true });
    ran.push({ name: step.name, status: r.status, stdout: r.stdout, stderr: r.stderr });
    const o = Object.fromEntries(readFileSync(out, 'latin1').split('\n').filter(Boolean).map((l) => l.split(/=(.*)/s).slice(0, 2)));
    if (step.id) outputs[step.id] = o;
    writeFileSync(out, '');
    if (r.status !== 0) { status = r.status; break; }
  }
  const calls = readFileSync(join(fake, 'calls.log'), 'utf8');
  return { status, ran, calls, outputs, uploaded: /^release upload /m.test(calls), last: (n) => ran.find((s) => s.name.startsWith(n)) };
}

const warnings = (r) => r.ran.flatMap((s) => s.stdout.split('\n')).filter((l) => l.startsWith('::warning'));

test('the steps are read from the workflow', () => {
  assert.deepEqual(STEPS.map((s) => s.name).slice(-4), ['Find the Latest release and its seq', 'Download the signed feed', 'Verify it', 'Upload it to Latest']);
});

test('a good newest feed on Latest sets the seq to beat and a newer feed is uploaded', () => {
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10) }, { tag: 'v1', feed: feed(BASE + 5) }], newFeed: feed(BASE + 20) });
  assert.equal(r.status, 0, JSON.stringify(r.ran));
  assert.deepEqual(warnings(r), []);
  assert.equal(r.outputs.latest.last, String(BASE + 10));
  assert.equal(r.uploaded, true);
  assert.match(r.calls, /release upload v2 /);
});

test('an unverifiable newest feed is skipped with a warning and the newest one that verifies is used', () => {
  // v2 is Latest and carries a signed file with a duplicate entry; v1 is good.
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10, { body: DUPLICATE }) }, { tag: 'v1', feed: feed(BASE + 5) }], newFeed: feed(BASE + 20) });
  assert.equal(r.status, 0, JSON.stringify(r.ran));
  assert.equal(warnings(r).length, 1);
  assert.match(warnings(r)[0], /::warning title=Version map on release v2 skipped::/);
  assert.match(r.last('Find').stderr, /refused: duplicate entry/);
  // v1 is not Latest, so the seq to beat is one below its own: Latest ends up with the current file.
  assert.equal(r.outputs.latest.last, String(BASE + 5 - 1));
  assert.equal(r.uploaded, true);
  assert.match(r.calls, /release upload v2 /);
});

test('a TEST-key file on a release is skipped the same way when the run did not ask for the test key', () => {
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10, { id: 'k1' }) }, { tag: 'v1', feed: feed(BASE + 5) }], newFeed: feed(BASE + 20) });
  assert.equal(r.status, 0, JSON.stringify(r.ran));
  assert.equal(warnings(r).length, 1);
  assert.equal(r.uploaded, true);
});

test('when no release has a feed that verifies, the run still stops unless first_publish is set', () => {
  const releases = [{ tag: 'v2', feed: feed(BASE + 10, { body: DUPLICATE }) }];
  const stopped = run({ releases, newFeed: feed(BASE + 20) });
  assert.equal(stopped.status, 1);
  assert.match(stopped.last('Find').stdout, /No release carries a feed/);
  assert.equal(stopped.uploaded, false);
  const first = run({ releases, newFeed: feed(BASE + 20), env: { FIRST_PUBLISH: 'true' } });
  assert.equal(first.status, 0, JSON.stringify(first.ran));
  assert.equal(first.uploaded, true);
});

test('a failure of the check itself is not skipped', () => {
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10) }], newFeed: feed(BASE + 20), brokenKeySource: true });
  assert.equal(r.status, 2);
  assert.deepEqual(warnings(r), []);
  assert.equal(r.uploaded, false);
});

test('a new feed that does not verify still fails the run and is not uploaded', () => {
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10) }], newFeed: feed(BASE + 20, { body: DUPLICATE }) });
  assert.equal(r.status, 3);
  assert.equal(r.uploaded, false);
});

test('allow_test_key against Latest is refused before anything is read or copied', () => {
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10) }], newFeed: feed(BASE + 20, { id: 'k1' }), env: { ALLOW_TEST_KEY: 'true' } });
  assert.equal(r.status, 1);
  assert.match(r.last('Find').stdout, /::error title=allow_test_key refused::.*v2, which is not a prerelease/);
  assert.equal(r.uploaded, false);
  assert.doesNotMatch(r.calls, /release download/);
});

test('allow_test_key is honoured when the release to copy to is a prerelease', () => {
  const r = run({ releases: [{ tag: 'v2-rc', feed: feed(BASE + 10, { id: 'k1' }) }], newFeed: feed(BASE + 20, { id: 'k1' }), env: { ALLOW_TEST_KEY: 'true' }, prerelease: true });
  assert.equal(r.status, 0, JSON.stringify(r.ran));
  assert.equal(r.uploaded, true);
});

test('a TEST-key file is refused when allow_test_key is not set', () => {
  const r = run({ releases: [{ tag: 'v2', feed: feed(BASE + 10) }], newFeed: feed(BASE + 20, { id: 'k1' }) });
  assert.equal(r.status, 3);
  assert.equal(r.uploaded, false);
});

test('the rehearsal release is refused as a source, whatever its case, before anything is read', () => {
  const releases = [{ tag: 'v2', feed: feed(BASE + 10) }];
  for (const name of ['vmap-feed-rehearsal', 'VMAP-Feed-Rehearsal']) {
    const r = run({ releases, newFeed: feed(BASE + 20), env: { FEED_RELEASE: name } });
    assert.equal(r.status, 1, name);
    assert.match(r.ran[0].stdout, /::error title=Rehearsal source refused::feed_release .*is the rehearsal prerelease/);
    assert.equal(r.ran.length, 1);
    assert.equal(r.calls, '');
  }
  const live = run({ releases, newFeed: feed(BASE + 20) });
  assert.equal(live.status, 0, JSON.stringify(live.ran));
  assert.equal(live.uploaded, true);
});

test('the rehearsal check is the first step that runs', () => {
  assert.equal(STEPS[0].name, 'Refuse a rehearsal as the source');
});
