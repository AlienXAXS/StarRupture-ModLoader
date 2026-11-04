// Tests for verify-feed.mjs.
//
//   node --test .github/vmap/verify-feed.test.mjs
//
// Keys are generated for the run. The loader's own TEST-key fixture and key
// source are used for the cross-check, so the script and the loader agree.

import test from 'node:test';
import assert from 'node:assert/strict';
import { createPrivateKey, generateKeyPairSync, sign } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { check, NotNewer, readHeaderBuilds, readKeySlots, Refused } from './verify-feed.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const TOOL = join(here, 'verify-feed.mjs');
const SOURCE = join(root, 'StarRupture-ModLoader-Core', 'memory_scanner', 'version_feed.cpp');
const NOW = 1_800_000_000;

const pair = () => {
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  return { pem: privateKey.export({ type: 'pkcs8', format: 'pem' }), hex: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32).toString('hex') };
};
const BODY = (seq) => `format\t1\nseq\t${seq}\nbuild\t128786\t0x22222222\t0x02000000\tpublic_test_branch 2\nfield\tATest::Native\t128786\t0x50\t0x4\t0x00\t-\n`;
const signed = (k, body, id = 'k1') => Buffer.from(`${body}sig\t${id}\t${sign(null, Buffer.from(body), createPrivateKey(k.pem)).toString('base64')}\n`);
const keys = (k, test = false) => ({ k1: { hex: k.hex, test } });
const HEADER = (rows) => `inline constexpr std::array<Build, ${rows.length}> kBuilds = {{\n${rows.map(([cl, st, sz]) => `\t{ ${cl}, 0x${st}, 0x${sz}, "x" },`).join('\n')}\n}};\n`;
const outcome = (fn) => { try { fn(); return 'ok'; } catch (e) { return e instanceof NotNewer ? `notnewer: ${e.message}` : e instanceof Refused ? `refused: ${e.message}` : `other: ${e.message}`; } };

test('accepts a good feed and reports its seq', () => {
  const k = pair();
  assert.deepEqual(check(signed(k, BODY(NOW)), { keys: keys(k), now: NOW }), { keyId: 'k1', seq: NOW, test: false });
});

test('a tampered file is refused', () => {
  const k = pair();
  const s = signed(k, BODY(NOW));
  const t = Buffer.from(s.toString('latin1').replace('0x50', '0x51'), 'latin1');
  assert.match(outcome(() => check(t, { keys: keys(k), now: NOW })), /^refused: the signature does not verify/);
});

test('an equal or lower seq is valid but not newer', () => {
  const k = pair();
  const s = signed(k, BODY(NOW));
  assert.match(outcome(() => check(s, { keys: keys(k), now: NOW, lastSeq: NOW })), /^notnewer/);
  assert.match(outcome(() => check(s, { keys: keys(k), now: NOW, lastSeq: NOW + 9 })), /^notnewer/);
  assert.equal(outcome(() => check(s, { keys: keys(k), now: NOW, lastSeq: NOW - 1 })), 'ok');
});

test('a tampered file with an equal seq is refused, not skipped', () => {
  const k = pair();
  const t = Buffer.from(signed(k, BODY(NOW)).toString('latin1').replace('0x50', '0x51'), 'latin1');
  assert.match(outcome(() => check(t, { keys: keys(k), now: NOW, lastSeq: NOW })), /^refused/);
});

test('refuses an unknown slot, a seq a day ahead, bad shape and an oversize file', () => {
  const k = pair();
  assert.match(outcome(() => check(signed(k, BODY(NOW), 'k2'), { keys: keys(k), now: NOW })), /no key in slot k2/);
  assert.match(outcome(() => check(signed(k, BODY(NOW + 86401)), { keys: keys(k), now: NOW })), /a day ahead/);
  assert.equal(outcome(() => check(signed(k, BODY(NOW + 86400)), { keys: keys(k), now: NOW })), 'ok');
  assert.match(outcome(() => check(signed(k, BODY(0)), { keys: keys(k), now: NOW })), /seq line/);
  assert.match(outcome(() => check(signed(k, 'seq\t5\nformat\t1\n'), { keys: keys(k), now: NOW })), /not a format 1/);
  assert.match(outcome(() => check(Buffer.alloc(1024 * 1024 + 1, 0x41), { keys: keys(k), now: NOW })), /bytes/);
  assert.match(outcome(() => check(signed(k, BODY(NOW)).subarray(0, -10), { keys: keys(k), now: NOW })), /LF|sig line/);
});

test('refuses an address-only row without the stamp, size and RVA', () => {
  const k = pair();
  const row = (rva) => `function	Test::MapOnly	0x00000000	128786	0x22222222	0x02000000	1	-	0	-1	-1	${rva}
`;
  assert.match(outcome(() => check(signed(k, BODY(NOW).replace('field', `${row('0x00000000')}field`)), { keys: keys(k), now: NOW })), /^refused: an address-only row/);
  assert.equal(outcome(() => check(signed(k, BODY(NOW).replace('field', `${row('0x00007000')}field`)), { keys: keys(k), now: NOW })), 'ok');
});

test('refuses a TEST-key feed when asked to', () => {
  const k = pair();
  const s = signed(k, BODY(NOW));
  assert.match(outcome(() => check(s, { keys: keys(k, true), now: NOW, refuseTestKey: true })), /TEST key/);
  assert.equal(outcome(() => check(s, { keys: keys(k, true), now: NOW })), 'ok');
});

test('the release gate wants every feed build in the header, with the same stamp and size', () => {
  const k = pair();
  const s = signed(k, BODY(NOW));
  const opts = { keys: keys(k), now: NOW };
  assert.equal(outcome(() => check(s, { ...opts, headerText: HEADER([[127004, '68F0A1B2', '0A3F2000'], [128786, '22222222', '02000000']]) })), 'ok');
  assert.match(outcome(() => check(s, { ...opts, headerText: HEADER([[127004, '68F0A1B2', '0A3F2000']]) })), /release gate: .*CL-128786/);
  assert.match(outcome(() => check(s, { ...opts, headerText: HEADER([[128786, '22222223', '02000000']]) })), /release gate/);
  assert.deepEqual([...readHeaderBuilds(HEADER([[127004, '68F0A1B2', '0A3F2000']])).keys()], [127004]);
});

test("reads the loader's own key slots, and the loader's TEST fixture verifies against them", () => {
  const slots = readKeySlots(readFileSync(SOURCE, 'utf8'));
  assert.deepEqual(Object.keys(slots), ['k1', 'k2']);
  assert.equal(slots.k1.test, true);
  assert.equal(slots.k2.test, false);
  const fx = readFileSync(join(root, 'Tests', 'VersionFeedTests', 'fixtures', 'test-feed-k1.txt'));
  assert.equal(check(fx, { keys: slots, now: 1761900000 }).test, true);
  assert.match(outcome(() => check(fx, { keys: slots, now: 1761900000, refuseTestKey: true })), /TEST key/);
});

test('the command line exits 0, 4 and 3, and prints the seq', () => {
  const k = pair();
  const dir = mkdtempSync(join(tmpdir(), 'vmap-verify-'));
  const src = join(dir, 'version_feed.cpp');
  writeFileSync(src, `constexpr const char* kK1Hex = "${k.hex}";\nconstexpr bool kK1TestOnly = false;\nconstexpr const char* kK2Hex = "";\n`);
  const feed = join(dir, 'feed.txt');
  writeFileSync(feed, signed(k, BODY(NOW)));
  const run = (...a) => spawnSync(process.execPath, [TOOL, feed, '--source', src, '--now', String(NOW), ...a], { encoding: 'utf8', windowsHide: true });
  const ok = run('--last-seq', String(NOW - 1));
  assert.equal(ok.status, 0, ok.stderr);
  assert.equal(ok.stdout, `${NOW}\n`);
  assert.equal(run('--last-seq', String(NOW)).status, 4);
  writeFileSync(feed, readFileSync(feed, 'latin1').replace('0x50', '0x51'), 'latin1');
  assert.equal(run('--last-seq', String(NOW)).status, 3);
  assert.equal(run('--bogus').status, 2);
});
