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

test('server_only is optional, and when present is true or false', () => {
  const ok = (server_only) => assert.equal(outcome(() => build([entry({ server_only })])), 'ok', JSON.stringify(server_only));
  const no = (server_only) => assert.match(outcome(() => build([entry({ server_only })])), /^refused: .*server_only must be true or false/, JSON.stringify(server_only));
  ok(true);
  ok(false);
  ok(undefined);
  no('true');
  no(1);
  no(null);
  no([true]);
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

const SHA = 'a'.repeat(64);
const ASSET = 'https://github.com/AlienXAXS/StarRupture-Plugin-Codex/releases/download/v1.2.0/';
const LATEST = 'https://github.com/AlienXAXS/StarRupture-Plugin-Codex/releases/latest/download/';
const installable = (over = {}) => entry({
  download_url: `${ASSET}Codex-Client.dll`, download_sha256: SHA, download_size: 10,
  install_files: ['Codex-Client.dll', 'Codex-Client.json'], update_manifest_url: `${LATEST}Codex-client-manifest.json`, ...over,
});

test('takes the install keys the loader takes', () => {
  assert.equal(outcome(() => build([installable()])), 'ok');
  assert.equal(outcome(() => build([installable({ install_files: ['Codex-Client.dll'], update_manifest_url: undefined })])), 'ok');
  assert.equal(outcome(() => build([installable({ download_size: 32 * 1024 * 1024 })])), 'ok');
});

test('refuses install keys the loader would drop', () => {
  const bad = [
    [{ download_url: undefined }, /download_url/],
    [{ download_url: `http://github.com/AlienXAXS/StarRupture-Plugin-Codex/releases/download/v1/Codex-Client.dll` }, /download_url/],
    [{ download_url: 'https://evil.example/o/r/releases/download/v1/Codex-Client.dll' }, /download_url/],
    [{ download_url: `${ASSET}Codex-Client.dll?x=1` }, /download_url/],
    [{ download_url: `${ASSET}../Codex-Client.dll` }, /download_url/],
    [{ download_url: `${ASSET}Codex-Server.dll` }, /download_url/],
    [{ download_url: `${ASSET}Codex.zip` }, /download_url/],
    [{ download_url: 'https://github.com/AlienXAXS/Other/releases/download/v1/Codex-Client.dll' }, /repository/],
    [{ page_url: 'https://www.nexusmods.com/starrupture/mods/1' }, /repository/],
    [{ download_sha256: 'A'.repeat(64) }, /download_sha256/],
    [{ download_sha256: `sha256:${SHA}` }, /download_sha256/],
    [{ download_size: 0 }, /download_size/],
    [{ download_size: '10' }, /download_size/],
    [{ download_size: 1.5 }, /download_size/],
    [{ download_size: 32 * 1024 * 1024 + 1 }, /download_size/],
    [{ install_files: [] }, /install_files/],
    [{ install_files: ['../Codex-Client.dll'] }, /install_files/],
    [{ install_files: ['CON.dll'] }, /install_files/],
    [{ install_files: ['.Codex.dll'] }, /install_files/],
    [{ install_files: ['Codex-Client.json'] }, /install_files/],
    [{ install_files: ['Codex-Client.dll', 'Other.json'] }, /install_files/],
    [{ install_files: ['Codex-Client.dll', 'Other.dll'] }, /install_files/],
    [{ update_manifest_url: undefined }, /update_manifest_url/],
    [{ install_files: ['Codex-Client.dll'] }, /update_manifest_url/],
    [{ update_manifest_url: `${ASSET}Codex-client-manifest.json` }, /update_manifest_url/],
    [{ update_manifest_url: `${LATEST}Codex.json` }, /update_manifest_url/],
    [{ update_manifest_url: 'https://github.com/AlienXAXS/Other/releases/latest/download/x-client-manifest.json' }, /update_manifest_url/],
  ];
  for (const [over, why] of bad) assert.match(outcome(() => build([installable(over)])), why, JSON.stringify(over));
});

test('every install entry of the real index names one version', () => {
  for (const e of JSON.parse(readFileSync(REAL_INDEX, 'utf8'))) {
    if (e.download_url) assert.ok(e.download_url.includes(`/releases/download/v${e.version}/`), e.id);
  }
});

test('a server_only entry has no install keys', () => {
  assert.match(outcome(() => build([installable({ server_only: true })])), /^refused: .*a server_only entry has no install keys/);
  assert.match(outcome(() => build([installable({ server_only: true, install_files: undefined, update_manifest_url: undefined })])), /^refused: .*server_only entry has no install keys/);
  assert.equal(outcome(() => build([entry({ server_only: true })])), 'ok');
  assert.equal(outcome(() => build([installable({ server_only: false })])), 'ok');
});

test('refuses what the loader would read differently from JSON.parse', () => {
  const body = text([installable()]);
  // download_size in exponent or fraction form: JSON.parse sees 2000000, the loader reads 2
  for (const size of ['2e6', '1.0e3', '10.0']) {
    assert.match(outcome(() => build(body.replace('"download_size": 10', `"download_size": ${size}`))), /^refused: download_size must be written as plain digits/, size);
  }
  // one .dll in install_files, not two of the same
  assert.match(outcome(() => build([installable({ install_files: ['Codex-Client.dll', 'Codex-Client.dll'], update_manifest_url: undefined })])), /^refused: .*exactly one plugin .dll name/);
  // a key repeated in the entry, or nested in another value
  const twice = body.replace('"download_size": 10,', '"download_size": 10,\n    "download_size": 20,');
  assert.match(outcome(() => build(twice)), /^refused: download_size must appear once/);
  assert.match(outcome(() => build([entry({ extra: installable() })])), /^refused: .*extra must be a plain value/);
  assert.match(outcome(() => build([entry({ nested: [1] })])), /^refused: .*nested must be a plain value/);
  assert.match(outcome(() => build([installable({ summary: 'a "download_size": 5 in text is fine' })])), /^ok$/);
});

const COMMIT = '0123456789abcdef0123456789abcdef01234567';
const PICTURE = `https://raw.githubusercontent.com/AlienXAXS/StarRupture-Plugin-Codex/${COMMIT}/images/thumbnail.png`;
const withPicture = (over = {}) => entry({ thumbnail_url: PICTURE, thumbnail_sha256: SHA, thumbnail_size: 183296, ...over });

test('takes the thumbnail keys the loader takes', () => {
  assert.equal(outcome(() => build([withPicture()])), 'ok');
  assert.equal(outcome(() => build([withPicture({ thumbnail_size: 1 })])), 'ok');
  assert.equal(outcome(() => build([withPicture({ thumbnail_size: 512 * 1024 })])), 'ok');
  // the owner and repository match the page_url's whatever the case, and the page may have a path
  assert.equal(outcome(() => build([withPicture({ page_url: 'https://github.com/alienxaxs/starrupture-plugin-codex/' })])), 'ok');
  assert.equal(outcome(() => build([withPicture({ page_url: 'https://github.com/AlienXAXS/StarRupture-Plugin-Codex/releases' })])), 'ok');
  // with install keys, and on a server_only entry
  assert.equal(outcome(() => build([installable(withPicture())])), 'ok');
  assert.equal(outcome(() => build([withPicture({ server_only: true })])), 'ok');
  // an entry with none of the three is as before
  assert.equal(outcome(() => build([entry()])), 'ok');
});

test('refuses thumbnail keys the signer or the loader would drop', () => {
  const repo = 'https://raw.githubusercontent.com/AlienXAXS/StarRupture-Plugin-Codex';
  const bad = [
    [{ thumbnail_url: undefined }, /all or none, missing thumbnail_url$/],
    [{ thumbnail_sha256: undefined }, /all or none, missing thumbnail_sha256$/],
    [{ thumbnail_size: undefined }, /all or none, missing thumbnail_size$/],
    [{ thumbnail_sha256: undefined, thumbnail_size: undefined }, /all or none, missing thumbnail_sha256, thumbnail_size$/],
    [{ thumbnail_url: `http://raw.githubusercontent.com/AlienXAXS/StarRupture-Plugin-Codex/${COMMIT}/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `https://github.com/AlienXAXS/StarRupture-Plugin-Codex/raw/${COMMIT}/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `https://example.test/AlienXAXS/StarRupture-Plugin-Codex/${COMMIT}/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/main/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/v1.2.0/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT.slice(1)}/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT.toUpperCase()}/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}0/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/images/thumbnail.png?x=1` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/images/thumbnail.png#x` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/images/thumbnail.png/` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/extra/images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/images/other.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/images/sub/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/Images/thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: `${repo}/${COMMIT}/images/../thumbnail.png` }, /thumbnail_url must be/],
    [{ thumbnail_url: 'https://raw.githubusercontent.com/AlienXAXS/Other/' + COMMIT + '/images/thumbnail.png' }, /belong to the GitHub repository/],
    [{ thumbnail_url: 'https://raw.githubusercontent.com/Else/StarRupture-Plugin-Codex/' + COMMIT + '/images/thumbnail.png' }, /belong to the GitHub repository/],
    [{ thumbnail_sha256: 'A'.repeat(64) }, /thumbnail_sha256/],
    [{ thumbnail_sha256: 'a'.repeat(63) }, /thumbnail_sha256/],
    [{ thumbnail_sha256: `sha256:${SHA}` }, /thumbnail_sha256/],
    [{ thumbnail_size: 0 }, /thumbnail_size/],
    [{ thumbnail_size: -5 }, /thumbnail_size/],
    [{ thumbnail_size: '10' }, /thumbnail_size/],
    [{ thumbnail_size: 1.5 }, /thumbnail_size/],
    [{ thumbnail_size: 512 * 1024 + 1 }, /thumbnail_size/],
    [{ thumbnail_url: null }, /thumbnail_url must be/],
  ];
  for (const [over, why] of bad) assert.match(outcome(() => build([withPicture(over)])), why, JSON.stringify(over));
});

test('a nexusmods.com entry has no thumbnail keys', () => {
  for (const page of ['https://www.nexusmods.com/starrupture/mods/1', 'https://nexusmods.com/starrupture/mods/1'])
    assert.match(outcome(() => build([withPicture({ page_url: page })])), /^refused: entry 1 \(codex\): thumbnail_url must belong to the GitHub repository page_url names; a nexusmods\.com entry has no repository/, page);
  assert.equal(outcome(() => build([entry({ page_url: 'https://www.nexusmods.com/starrupture/mods/1' })])), 'ok');
  // the same picture on a GitHub page is fine
  assert.equal(outcome(() => build([withPicture()])), 'ok');
});

test('refuses thumbnail keys the loader would read differently from JSON.parse', () => {
  const body = text([withPicture()]);
  for (const size of ['2e5', '1.0e3', '10.0'])
    assert.match(outcome(() => build(body.replace('"thumbnail_size": 183296', `"thumbnail_size": ${size}`))), /^refused: thumbnail_size must be written as plain digits/, size);
  const twice = body.replace('"thumbnail_size": 183296', '"thumbnail_size": 183296,\n    "thumbnail_size": 20');
  assert.match(outcome(() => build(twice)), /^refused: thumbnail_size must appear once/);
  assert.match(outcome(() => build([entry({ extra: withPicture() })])), /^refused: .*extra must be a plain value/);
  assert.match(outcome(() => build([entry({ extra: [{ thumbnail_url: PICTURE }] })])), /^refused: .*extra must be a plain value/);
  assert.match(outcome(() => build([withPicture({ summary: 'a "thumbnail_size": 5 in text is fine' })])), /^ok$/);
});
