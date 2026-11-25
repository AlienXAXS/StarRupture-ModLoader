#!/usr/bin/env node
// Checks mod-index/index.json and builds the unsigned file the signer takes.
// The publish side and a contributor's own check use the same script.
//
//   node build-mod-index.mjs <index.json> --out <file> [--seq <n>]
//
// Without --seq the output is the signer's input: the lines `format<TAB>1` and
// `kind<TAB>mod-index`, then the index text. The signer inserts the `seq` line
// after `format` and appends `sig<TAB><keyId><TAB><base64>`, a signature over
// every byte before that line (see ci/vmap-sign.mjs `sign` in the feed repo).
// With --seq the output is the whole file as it is published except for the
// sig line: `format`, `seq`, `kind`, then the index text. That is for a local
// look at the final layout; nothing signs it.
//
// The checks are the loader's own (mod_index.h), so an index that passes here
// is not cut down when the loader reads it:
//   - at most 256 KiB, with room left for the header and sig lines
//   - UTF-8 with LF line endings (any CR is refused) and a final LF
//   - a JSON array of at most 200 entries
//   - every entry has an id, a name and a page_url that is https on
//     nexusmods.com or github.com; a readme_url, when present, is https on
//     raw.githubusercontent.com
//   - requires, when present, is an array of at most 8 ids, each a string of at
//     most 64 bytes (never objects: the loader would read their keys as the
//     entry's)
//   - the install keys, when any is present, are all valid (see checkInstall)
//
// Exit codes: 0 built; 3 the index is refused; 2 usage error.

import { readFileSync, writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export const EXIT_OK = 0;
export const EXIT_ERROR = 2;
export const EXIT_REFUSED = 3;
export const MAX_ENTRIES = 200;
// The whole published file is capped at 256 KiB; the sig line and the header
// lines take under 256 bytes of it.
export const MAX_BODY_BYTES = 256 * 1024 - 256;
const PAGE_HOSTS = ['nexusmods.com', 'www.nexusmods.com', 'github.com', 'www.github.com'];
const README_HOSTS = ['raw.githubusercontent.com'];
const MAX_REQUIRES = 8;
const MAX_ID_BYTES = 64;

export class Refused extends Error {}
const need = (cond, msg) => { if (!cond) throw new Refused(msg); };

// The loader's URL rule (IsAllowedUrl in mod_index.h), word for word: https, at
// most 200 bytes, only printable ASCII, no backslash, no user info or port, and
// a host equal to one of the allowed hosts (case does not matter).
const MAX_URL_LEN = 200;
function allowedUrl(value, hosts) {
  if (typeof value !== 'string' || !value.startsWith('https://') || value.length <= 'https://'.length || value.length > MAX_URL_LEN) return false;
  if (/[^!-~]|\\/.test(value)) return false;
  const end = value.slice('https://'.length).search(/[/?#]/);
  const host = end === -1 ? value.slice('https://'.length) : value.slice('https://'.length, 'https://'.length + end);
  if (/[@:]/.test(host)) return false;
  return hosts.includes(host.toLowerCase());
}

// The loader trims control characters and spaces from a name and an id, and drops an entry left with none.
const hasText = (v) => typeof v === 'string' && v.replace(/[\x00-\x20\x7f]/g, '') !== '';

const allowedRequires = (value) => Array.isArray(value) && value.length <= MAX_REQUIRES &&
  value.every((id) => hasText(id) && Buffer.byteLength(id) <= MAX_ID_BYTES);

// The loader's install rules (ReadInstall and IsReleaseAssetUrl in mod_index.h).
// The loader lists an entry that breaks one and offers no Install; here it is refused.
const INSTALL_KEYS = ['download_url', 'download_sha256', 'download_size', 'install_files', 'update_manifest_url'];
const MAX_DOWNLOAD_BYTES = 32 * 1024 * 1024;
const MAX_DLL_NAME_LEN = 80;

// ASCII letters, digits, '_', '.', '-', with no "..".
const plainSegment = (s) => /^[A-Za-z0-9_.-]+$/.test(s) && s !== '.' && !s.includes('..');
const sameName = (a, b) => a.toLowerCase() === b.toLowerCase();
const endsWith = (s, tail) => s.toLowerCase().endsWith(tail.toLowerCase());
const installDllName = (name) => typeof name === 'string' && name.length <= MAX_DLL_NAME_LEN && plainSegment(name) &&
  !name.startsWith('.') && name.length > 4 && endsWith(name, '.dll') && !endsWith(name, '-Server.dll') &&
  !/^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\.|$)/i.test(name);

// https://github.com/<owner>/<repo>/releases/download/<tag>/<name>.dll, as its parts, or null.
function releaseAsset(url) {
  const start = 'https://github.com/';
  if (typeof url !== 'string' || url.length > MAX_URL_LEN || !url.startsWith(start)) return null;
  const p = url.slice(start.length).split('/');
  return p.length === 6 && p[2] === 'releases' && p[3] === 'download' && p.every(plainSegment) && installDllName(p[5]) ? p : null;
}

function checkInstall(e, who) {
  if (INSTALL_KEYS.every((key) => e[key] === undefined)) return;
  const asset = releaseAsset(e.download_url);
  need(asset, `${who}: download_url must be the .dll asset of a GitHub release`);
  const page = e.page_url.split('/'); // https: "" host owner repo ...
  need(page.length >= 5 && sameName(page[2], 'github.com') && sameName(page[3], asset[0]) && sameName(page[4], asset[1]),
    `${who}: download_url must belong to page_url's repository`);
  need(typeof e.download_sha256 === 'string' && /^[0-9a-f]{64}$/.test(e.download_sha256),
    `${who}: download_sha256 must be 64 lowercase hex characters`);
  need(Number.isInteger(e.download_size) && e.download_size >= 1 && e.download_size <= MAX_DOWNLOAD_BYTES,
    `${who}: download_size must be a whole number from 1 to ${MAX_DOWNLOAD_BYTES}`);

  const files = e.install_files;
  need(Array.isArray(files) && files.length >= 1 && files.length <= 2 && files.every((f) => typeof f === 'string'),
    `${who}: install_files must be one .dll name, with or without its .json`);
  const dll = files.find(installDllName);
  need(dll, `${who}: install_files has no plugin .dll name`);
  const json = files.filter((f) => f !== dll);
  need(json.length === 0 || (json.length === 1 && sameName(json[0], `${dll.slice(0, -4)}.json`)),
    `${who}: install_files may only add ${dll.slice(0, -4)}.json`);

  const manifest = e.update_manifest_url;
  need((json.length === 1) === (typeof manifest === 'string' && manifest !== ''),
    `${who}: update_manifest_url goes with the .json in install_files, and only with it`);
  if (json.length === 0) return;
  const latest = `https://github.com/${asset[0]}/${asset[1]}/releases/latest/download/`;
  const name = manifest.slice(latest.length);
  need(manifest.length <= MAX_URL_LEN && sameName(manifest.slice(0, latest.length), latest) &&
    plainSegment(name) && endsWith(name, '-client-manifest.json'),
    `${who}: update_manifest_url must be a -client-manifest.json of the repository's latest release`);
}

// Throws Refused with the reason, naming the entry, when the index is not fit to publish.
export function checkIndex(bytes) {
  need(bytes.length > 0, 'the index is empty');
  need(bytes.length <= MAX_BODY_BYTES, `the index is ${bytes.length} bytes (at most ${MAX_BODY_BYTES})`);
  need(!bytes.includes(0x0d), 'the index has a CR; save it with LF line endings');
  need(bytes[bytes.length - 1] === 0x0a, 'the index does not end in a newline');
  let list;
  try {
    list = JSON.parse(new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes));
  } catch (e) {
    throw new Refused(`the index is not valid UTF-8 JSON: ${e.message}`);
  }
  need(Array.isArray(list), 'the index is not a JSON array');
  need(list.length <= MAX_ENTRIES, `the index has ${list.length} entries (at most ${MAX_ENTRIES})`);
  list.forEach((e, i) => {
    const who = `entry ${i + 1}${typeof e?.id === 'string' && e.id !== '' ? ` (${e.id})` : ''}`;
    need(e !== null && typeof e === 'object' && !Array.isArray(e), `${who} is not an object`);
    need(hasText(e.id), `${who} has no id`);
    need(hasText(e.name), `${who} has no name`);
    need(allowedUrl(e.page_url, PAGE_HOSTS), `${who}: page_url must be https on nexusmods.com or github.com`);
    need(e.readme_url === undefined || allowedUrl(e.readme_url, README_HOSTS), `${who}: readme_url must be https on raw.githubusercontent.com`);
    need(e.requires === undefined || allowedRequires(e.requires), `${who}: requires must be at most ${MAX_REQUIRES} ids, each a string of at most ${MAX_ID_BYTES} bytes`);
    checkInstall(e, who);
  });
}

// bytes: the index file. Returns the file to hand to the signer, or with seq
// the full envelope minus the sig line.
export function buildModIndex(bytes, { seq = null } = {}) {
  checkIndex(bytes);
  if (seq !== null) need(Number.isSafeInteger(seq) && seq >= 1, 'seq is a positive integer');
  const head = seq === null ? 'format\t1\nkind\tmod-index\n' : `format\t1\nseq\t${seq}\nkind\tmod-index\n`;
  return Buffer.concat([Buffer.from(head), bytes]);
}

function main(argv) {
  const opt = {};
  const rest = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (['--out', '--seq'].includes(a) && i + 1 < argv.length) opt[a.slice(2)] = argv[++i];
    else if (a.startsWith('--')) throw new Error(`unknown or incomplete option ${a}`);
    else rest.push(a);
  }
  if (rest.length !== 1 || !opt.out) throw new Error('usage: build-mod-index.mjs <index.json> --out <file> [--seq n]');
  if (opt.seq !== undefined && !(/^[1-9][0-9]*$/.test(opt.seq) && Number.isSafeInteger(Number(opt.seq)))) throw new Error('--seq is a positive integer');
  try {
    const built = buildModIndex(readFileSync(rest[0]), { seq: opt.seq === undefined ? null : Number(opt.seq) });
    writeFileSync(opt.out, built);
    process.stderr.write(`build-mod-index: ok, ${built.length} bytes${opt.seq === undefined ? ' (signer input)' : `, seq ${opt.seq}`}\n`);
    return EXIT_OK;
  } catch (e) {
    if (e instanceof Refused) { process.stderr.write(`build-mod-index: refused: ${e.message}\n`); return EXIT_REFUSED; }
    throw e;
  }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (e) {
    process.stderr.write(`build-mod-index: ${e.message}\n`);
    process.exitCode = EXIT_ERROR;
  }
}
