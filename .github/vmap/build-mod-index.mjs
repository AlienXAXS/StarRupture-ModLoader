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

export class Refused extends Error {}
const need = (cond, msg) => { if (!cond) throw new Refused(msg); };

// A string that is an https URL on one of the hosts: no port, no user name, no
// backslash and no space, as the loader requires.
function allowedUrl(value, hosts) {
  if (typeof value !== 'string' || /[\\\s]/.test(value) || !value.startsWith('https://')) return false;
  let u;
  try { u = new URL(value); } catch { return false; }
  return u.protocol === 'https:' && u.port === '' && u.username === '' && u.password === '' && hosts.includes(u.hostname);
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
    need(typeof e.id === 'string' && e.id !== '', `${who} has no id`);
    need(typeof e.name === 'string' && e.name !== '', `${who} has no name`);
    need(allowedUrl(e.page_url, PAGE_HOSTS), `${who}: page_url must be https on nexusmods.com or github.com`);
    need(e.readme_url === undefined || allowedUrl(e.readme_url, README_HOSTS), `${who}: readme_url must be https on raw.githubusercontent.com`);
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
  if (opt.seq !== undefined && !/^[1-9][0-9]*$/.test(opt.seq)) throw new Error('--seq is a positive integer');
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
