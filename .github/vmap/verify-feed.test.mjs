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

test('the flavor rule: a server file names only server exes, a client file none', () => {
  const k = pair();
  const opts = { keys: keys(k), now: NOW };
  const SERVER = (seq) => BODY(seq).replace('public_test_branch 2', 'server steam_build 25642172');
  const MIXED = (seq) => SERVER(seq).replace('field', 'build\t128787\t0x33333333\t0x03000000\tsteam_build 1\nfield');
  const NONE = (seq) => `format\t1\nseq\t${seq}\n`;
  const client = (body) => outcome(() => check(signed(k, body), { ...opts, flavor: 'client' }));
  const server = (body) => outcome(() => check(signed(k, body), { ...opts, flavor: 'server' }));
  assert.equal(client(BODY(NOW)), 'ok');
  assert.equal(server(SERVER(NOW)), 'ok');
  assert.match(client(SERVER(NOW)), /^refused: a dedicated server file, not a client file/);
  assert.match(server(BODY(NOW)), /^refused: not a dedicated server file \(a build line is not labelled/);
  assert.match(server(MIXED(NOW)), /^refused: not a dedicated server file \(a build line is not labelled/);
  assert.match(client(MIXED(NOW)), /^refused: a dedicated server file/);
  assert.match(server(NONE(NOW)), /^refused: not a dedicated server file \(it has no build line\)/);
  assert.equal(client(NONE(NOW)), 'ok');
  // No flavor given: the old behavior, no label check.
  assert.equal(outcome(() => check(signed(k, SERVER(NOW)), opts)), 'ok');
  assert.equal(outcome(() => check(signed(k, BODY(NOW)), opts)), 'ok');
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
  assert.equal(run('--flavor', 'both').status, 2);
  // A client file under the server flavor is refused (3); a server file the other way too.
  writeFileSync(feed, signed(k, BODY(NOW)));
  assert.equal(run('--flavor', 'client').status, 0);
  const refused = run('--flavor', 'server');
  assert.equal(refused.status, 3);
  assert.match(refused.stderr, /not a dedicated server file/);
  writeFileSync(feed, signed(k, BODY(NOW).replace('public_test_branch 2', 'server steam_build 25642172')));
  assert.equal(run('--flavor', 'server').status, 0);
  assert.equal(run('--flavor', 'client').status, 3);
});

// The signed mod index: format 1, seq, `kind mod-index`, the index JSON, the sig line.
const INDEX = '[{"id":"codex","name":"Codex","page_url":"https://github.com/AlienXAXS/StarRupture-Plugin-Codex"}]\n';
const MI = (seq, body = INDEX, kind = 'mod-index') => `format\t1\nseq\t${seq}\nkind\t${kind}\n${body}`;
const MI_CAP = 256 * 1024;

test('mod index: accepts a good file and reports its seq', () => {
  const k = pair();
  const opts = { keys: keys(k), now: NOW, kind: 'mod-index' };
  assert.deepEqual(check(signed(k, MI(NOW)), opts), { keyId: 'k1', seq: NOW, test: false });
  assert.deepEqual(check(signed(k, MI(NOW)), { ...opts, keys: keys(k, true) }), { keyId: 'k1', seq: NOW, test: true });
});

test('mod index: a tampered body, a wrong key id and a missing or cut sig line are refused', () => {
  const k = pair();
  const opts = { keys: keys(k), now: NOW, kind: 'mod-index' };
  const s = signed(k, MI(NOW));
  assert.match(outcome(() => check(Buffer.from(s.toString('latin1').replace('Codex', 'Kodex'), 'latin1'), opts)), /^refused: the signature does not verify/);
  assert.match(outcome(() => check(signed(k, MI(NOW), 'k2'), opts)), /^refused: no key in slot k2/);
  assert.match(outcome(() => check(Buffer.from(MI(NOW)), opts)), /^refused: the last line is not a sig line/);
  assert.match(outcome(() => check(s.subarray(0, -10), opts)), /^refused: the file does not end in LF/);
  assert.match(outcome(() => check(s.subarray(0, -1), opts)), /^refused: the (file does not end in LF|last line is not a sig line)/);
  assert.match(outcome(() => check(Buffer.alloc(0), opts)), /^refused: the file is 0 bytes/);
});

test('mod index: the kind line, the body and the size are checked', () => {
  const k = pair();
  const opts = { keys: keys(k), now: NOW, kind: 'mod-index' };
  assert.match(outcome(() => check(signed(k, MI(NOW, INDEX, 'catalog')), opts)), /^refused: the third line is not `kind mod-index`/);
  assert.match(outcome(() => check(signed(k, BODY(NOW)), opts)), /^refused: the third line/);
  assert.match(outcome(() => check(signed(k, `format\t1\nseq\t${NOW}\n${INDEX}`), opts)), /^refused: the third line/);
  assert.match(outcome(() => check(signed(k, MI(NOW, '')), opts)), /^refused: the mod index is empty/);
  assert.match(outcome(() => check(signed(k, MI(NOW, '[{"id":\n')), opts)), /^refused: the mod index is not UTF-8 JSON/);
  assert.match(outcome(() => check(signed(k, MI(NOW, '﻿[]\n')), opts)), /^refused: the mod index is not UTF-8 JSON/);
  assert.match(outcome(() => check(signed(k, 'format\t2\nseq\t1\nkind\tmod-index\n[]\n'), opts)), /not a format 1/);
  // The whole file, sig line included, is at most 256 KiB. A version map feed may be larger.
  const pad = (n) => signed(k, MI(NOW, `[${' '.repeat(n)}]\n`));
  const room = MI_CAP - pad(0).length;
  assert.equal(pad(room).length, MI_CAP);
  assert.equal(outcome(() => check(pad(room), opts)), 'ok');
  assert.match(outcome(() => check(pad(room + 1), opts)), /^refused: the file is 262145 bytes \(1 to 262144 allowed\)/);
  assert.equal(outcome(() => check(signed(k, `${BODY(NOW)}note\t${'x'.repeat(MI_CAP)}\n`), { keys: keys(k), now: NOW })), 'ok');
});

test('mod index: an old seq is valid but not newer, a tampered one is still refused', () => {
  const k = pair();
  const opts = { keys: keys(k), now: NOW, kind: 'mod-index' };
  const s = signed(k, MI(NOW));
  assert.match(outcome(() => check(s, { ...opts, lastSeq: NOW })), /^notnewer/);
  assert.match(outcome(() => check(s, { ...opts, lastSeq: NOW + 9 })), /^notnewer/);
  assert.equal(outcome(() => check(s, { ...opts, lastSeq: NOW - 1 })), 'ok');
  assert.match(outcome(() => check(Buffer.from(s.toString('latin1').replace('Codex', 'Kodex'), 'latin1'), { ...opts, lastSeq: NOW })), /^refused/);
  assert.match(outcome(() => check(signed(k, MI(NOW + 86401)), opts)), /a day ahead/);
});

test('mod index: a TEST-key file is refused when asked to', () => {
  const k = pair();
  assert.match(outcome(() => check(signed(k, MI(NOW)), { keys: keys(k, true), now: NOW, kind: 'mod-index', refuseTestKey: true })), /TEST key/);
});

test('mod index: the command line exits 0, 4 and 3 and refuses options that do not apply', () => {
  const k = pair();
  const dir = mkdtempSync(join(tmpdir(), 'vmap-verify-mi-'));
  const src = join(dir, 'version_feed.cpp');
  writeFileSync(src, `constexpr const char* kK1Hex = "${k.hex}";\nconstexpr bool kK1TestOnly = false;\nconstexpr const char* kK2Hex = "";\n`);
  const file = join(dir, 'mod-index.v1.txt');
  writeFileSync(file, signed(k, MI(NOW)));
  const run = (...a) => spawnSync(process.execPath, [TOOL, file, '--source', src, '--now', String(NOW), ...a], { encoding: 'utf8', windowsHide: true });
  const ok = run('--kind', 'mod-index', '--last-seq', String(NOW - 1));
  assert.equal(ok.status, 0, ok.stderr);
  assert.equal(ok.stdout, `${NOW}\n`);
  assert.equal(run('--kind', 'mod-index', '--last-seq', String(NOW)).status, 4);
  assert.equal(run('--kind', 'mod-index', '--flavor', 'client').status, 2);
  assert.equal(run('--kind', 'nope').status, 2);
  // A version map feed is not a mod index, and the other way round the feed path is unchanged.
  writeFileSync(file, signed(k, BODY(NOW)));
  assert.equal(run('--kind', 'mod-index').status, 3);
  assert.equal(run('--kind', 'feed').status, 0);
  writeFileSync(file, readFileSync(file, 'latin1').replace('0x50', '0x51'), 'latin1');
  assert.equal(run('--kind', 'mod-index').status, 3);
});
