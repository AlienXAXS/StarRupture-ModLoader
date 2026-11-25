// Tests for build-mod-index.mjs.
//
//   node --test .github/vmap/build-mod-index.test.mjs

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { MAX_BODY_BYTES, MAX_ENTRIES, Refused, buildModIndex } from './build-mod-index.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const TOOL = join(here, 'build-mod-index.mjs');
const REAL_INDEX = join(here, '..', '..', 'mod-index', 'index.json');

const entry = (over = {}) => ({ id: 'codex', name: 'Codex', page_url: 'https://github.com/AlienXAXS/StarRupture-Plugin-Codex', ...over });
const text = (list) => `${JSON.stringify(list, null, 2)}\n`;
const build = (list, opts) => buildModIndex(Buffer.from(typeof list === 'string' ? list : text(list)), opts);
const outcome = (fn) => { try { fn(); return 'ok'; } catch (e) { return e instanceof Refused ? `refused: ${e.message}` : `other: ${e.message}`; } };

test('builds the signer input: format and kind lines, then the index text byte for byte', () => {
  const body = text([entry()]);
  assert.equal(build(body).toString(), `format\t1\nkind\tmod-index\n${body}`);
});

test('with a seq it builds the full file up to the sig line', () => {
  const body = text([entry()]);
  assert.equal(build(body, { seq: 7 }).toString(), `format\t1\nseq\t7\nkind\tmod-index\n${body}`);
  assert.match(outcome(() => build(body, { seq: 0 })), /^refused: seq is a positive integer/);
  assert.match(outcome(() => build(body, { seq: 1.5 })), /^refused: seq is a positive integer/);
});

test('refuses a file that is not fit to publish', () => {
  assert.match(outcome(() => build('')), /^refused: the index is empty/);
  assert.match(outcome(() => build(text([entry()]).replace(/\n/g, '\r\n'))), /^refused: the index has a CR/);
  assert.match(outcome(() => build(text([entry()]).trimEnd())), /^refused: the index does not end in a newline/);
  assert.match(outcome(() => build('[{"id":\n')), /^refused: the index is not valid UTF-8 JSON/);
  assert.match(outcome(() => buildModIndex(Buffer.from([0x5b, 0xff, 0x5d, 0x0a]))), /^refused: the index is not valid UTF-8 JSON/);
  assert.match(outcome(() => build('﻿[]\n')), /^refused: the index is not valid UTF-8 JSON/);
  assert.match(outcome(() => build('{"id":"codex"}\n')), /^refused: the index is not a JSON array/);
  assert.match(outcome(() => build(['codex'])), /^refused: entry 1 is not an object/);
});

test('refuses an index over 256 KiB or over 200 entries', () => {
  const list = (n) => Array.from({ length: n }, (_, i) => entry({ id: `p${i}` }));
  assert.equal(outcome(() => build(list(MAX_ENTRIES))), 'ok');
  assert.match(outcome(() => build(list(MAX_ENTRIES + 1))), /^refused: the index has 201 entries/);
  const pad = (n) => `[${' '.repeat(n)}]\n`;
  assert.equal(outcome(() => build(pad(MAX_BODY_BYTES - 3))), 'ok');
  assert.match(outcome(() => build(pad(MAX_BODY_BYTES - 2))), /^refused: the index is \d+ bytes/);
});

test('every entry needs an id, a name and a page_url on an allowed https host', () => {
  assert.match(outcome(() => build([entry({ id: undefined })])), /^refused: entry 1 has no id/);
  assert.match(outcome(() => build([entry(), entry({ id: 'x', name: '' })])), /^refused: entry 2 \(x\) has no name/);
  assert.match(outcome(() => build([entry({ page_url: undefined })])), /^refused: entry 1 \(codex\): page_url/);
  const ok = ['https://github.com/a/b', 'https://www.github.com/a/b', 'https://nexusmods.com/starrupture/mods/1', 'https://www.nexusmods.com/starrupture/mods/1'];
  for (const u of ok) assert.equal(outcome(() => build([entry({ page_url: u })])), 'ok', u);
  const bad = ['http://github.com/a/b', 'https://evil.example/a', 'https://github.com.evil.example/a', 'https://github.com:8443/a', 'https://me@github.com/a',
    'https://github.com\\a', 'https://github.com/a b', 'https://raw.githubusercontent.com/a/b', 'ftp://github.com/a', 'github.com/a', 42,
    'https://github.com:443/a', 'https://github.com:/a', 'https://@github.com/a', 'https:///github.com/a', 'https://github.com/café',
    'https://github.com/a\tb', `https://github.com/${'a'.repeat(200 - 'https://github.com/'.length + 1)}`, 'https://'];
  assert.equal(outcome(() => build([entry({ page_url: `https://github.com/${'a'.repeat(200 - 'https://github.com/'.length)}` })])), 'ok');
  assert.equal(outcome(() => build([entry({ page_url: 'https://GitHub.com/a/b?x=1#y' })])), 'ok');
  assert.match(outcome(() => build([entry({ name: ' \t ' })])), /^refused: entry 1 \(codex\) has no name/);
  for (const u of bad) assert.match(outcome(() => build([entry({ page_url: u })])), /^refused: .*page_url must be https on nexusmods.com or github.com/, String(u));
});

test('a readme_url is optional, and when present is https on raw.githubusercontent.com', () => {
  assert.equal(outcome(() => build([entry({ readme_url: 'https://raw.githubusercontent.com/a/b/main/README.md' })])), 'ok');
  assert.match(outcome(() => build([entry({ readme_url: 'https://github.com/a/b/blob/main/README.md' })])), /^refused: .*readme_url must be https on raw.githubusercontent.com/);
  assert.match(outcome(() => build([entry({ readme_url: 'http://raw.githubusercontent.com/a' })])), /^refused: .*readme_url/);
});

test('requires is optional, and when present is a short list of short ids, strings only', () => {
  const ok = (requires) => assert.equal(outcome(() => build([entry({ requires })])), 'ok', JSON.stringify(requires));
  const no = (requires) => assert.match(outcome(() => build([entry({ requires })])), /^refused: .*requires must be at most 8 ids/, JSON.stringify(requires));
  ok([]);
  ok(['minimap']);
  ok(Array.from({ length: 8 }, (_, i) => `p${i}`));
  ok(['x'.repeat(64)]);
  no(Array.from({ length: 9 }, (_, i) => `p${i}`));
  no(['x'.repeat(65)]);
  no(['é'.repeat(33)]);
  no('minimap');
  no(null);
  no({ id: 'minimap' });
  no([{ id: 'minimap' }]);
  no([['minimap']]);
  no([5]);
  no(['']);
  no(['  ']);
});

test('the index in the repository builds, and the command line writes the file', () => {
  const dir = mkdtempSync(join(tmpdir(), 'build-mod-index-'));
  const out = join(dir, 'mod-index.txt');
  const run = (...a) => spawnSync(process.execPath, [TOOL, ...a], { encoding: 'utf8', windowsHide: true });
  const r = run(REAL_INDEX, '--out', out);
  assert.equal(r.status, 0, r.stderr);
  const built = readFileSync(out);
  assert.equal(built.subarray(0, 'format\t1\nkind\tmod-index\n'.length).toString(), 'format\t1\nkind\tmod-index\n');
  assert.ok(built.subarray('format\t1\nkind\tmod-index\n'.length).equals(readFileSync(REAL_INDEX)));
  const withSeq = run(REAL_INDEX, '--out', out, '--seq', '1800000000');
  assert.equal(withSeq.status, 0, withSeq.stderr);
  assert.ok(readFileSync(out).toString().startsWith('format\t1\nseq\t1800000000\nkind\tmod-index\n'));
  const badFile = join(dir, 'bad.json');
  writeFileSync(badFile, '[{"id":"x","name":"X","page_url":"https://evil.example"}]\n');
  const refused = run(badFile, '--out', join(dir, 'never.txt'));
  assert.equal(refused.status, 3);
  assert.match(refused.stderr, /page_url must be https/);
  assert.equal(run(REAL_INDEX).status, 2);
  assert.equal(run(REAL_INDEX, '--out', out, '--seq', '0').status, 2);
  assert.equal(run(REAL_INDEX, '--out', out, '--bogus').status, 2);
});
