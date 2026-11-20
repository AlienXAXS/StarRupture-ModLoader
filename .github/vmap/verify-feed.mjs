#!/usr/bin/env node
// Checks a signed version-map feed before it is attached to a release. The
// publish workflow and the release workflow both use it.
//
//   node verify-feed.mjs <signed-feed> --source <version_feed.cpp>
//        [--last-seq <n>] [--now <unix seconds>] [--refuse-test-key]
//        [--header <version_map.generated.h>]
//
// The public keys are read from the loader's own key slots (kK1Hex, kK2Hex in
// version_feed.cpp), so the loader and this check cannot disagree about them.
// The signature covers every byte before the last line, which is
// `sig<TAB><keyId><TAB><base64>` (Ed25519, RFC 8032). The file is at most
// 1 MiB, starts with `format 1` and `seq <n>`, and its `seq` is not more than a
// day ahead of the clock. A game build (CL) listed twice, or a function, absent
// or field row listed twice for one CL, is refused: the loader refuses the whole
// file for it (see findDuplicate).
//
// --last-seq: the `seq` of the file now on the release. A `seq` that does not
//   rise above it is not an error but is not published either (exit 4).
// --refuse-test-key: refuse a feed signed with a slot the source flags as a
//   TEST key (kK<n>TestOnly = true).
// --header: the release gate. Every build in the feed must be in the header's
//   build table with the same image stamp and size, or a release built from
//   this header would know fewer game builds than the feed it ships with.
//
// Exit codes: 0 publish it; 3 refused (tampered, malformed, unknown key, test
// key, gate); 4 valid but not newer; 2 usage error. Prints the `seq` on success.

import { createPublicKey, verify } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export const EXIT_OK = 0;
export const EXIT_ERROR = 2;
export const EXIT_REFUSED = 3;
export const EXIT_NOT_NEWER = 4;
const CAP_BYTES = 1024 * 1024;
const MAX_SEQ_AHEAD = 24 * 60 * 60;

export class Refused extends Error {}
export class NotNewer extends Error {}
const need = (cond, msg) => { if (!cond) throw new Refused(msg); };

const SPKI_PREFIX = Buffer.from('302a300506032b6570032100', 'hex');

// { k1: { hex, test }, k2: { hex, test } } for each slot that holds a key.
export function readKeySlots(cppText) {
  const keys = {};
  for (const slot of ['1', '2']) {
    const hex = new RegExp(`kK${slot}Hex\\s*=\\s*"([0-9a-f]*)"`).exec(cppText);
    if (!hex) throw new Error(`kK${slot}Hex not found in the key source`);
    const test = new RegExp(`kK${slot}TestOnly\\s*=\\s*(true|false)`).exec(cppText);
    if (hex[1] !== '') keys[`k${slot}`] = { hex: hex[1], test: test?.[1] === 'true' };
  }
  return keys;
}

// The build table of the header: { cl -> "0xSTAMP 0xSIZE" }.
export function readHeaderBuilds(headerText) {
  const block = /kBuilds\s*=\s*\{\{([\s\S]*?)\}\};/.exec(headerText);
  if (!block) throw new Error('kBuilds not found in the header');
  const builds = new Map();
  for (const m of block[1].matchAll(/\{\s*(\d+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*,/g)) {
    builds.set(Number(m[1]), `${parseInt(m[2], 16)} ${parseInt(m[3], 16)}`);
  }
  return builds;
}

// The first thing in the feed the loader would see twice, or null. The loader
// refuses the whole file for it (ParseFeed in version_map.cpp), so one slip
// switches the version object map off for every player. The same rules, with the
// loader's lines:
//   - two `build` lines for one CL (version_map.cpp:347-349);
//   - two `function` or `absent` rows for one (key, fp, CL), the two kinds
//     sharing one set (:315, :369-370);
//   - two `field` rows for one (key, CL) (:316, :401-402).
// Only rows the loader would read are counted: a known kind with too few columns,
// or a number that is not written the way the loader reads it, is refused by the
// loader for another reason. Numbers compare by value, as the loader's do.
export function findDuplicate(lines) {
  const dec = (s) => (/^[1-9][0-9]{0,9}$/.test(s ?? '') ? Number(s) : null);
  const hex = (s) => (/^0x[0-9A-F]{1,8}$/.test(s ?? '') ? parseInt(s, 16) : null);
  const seen = { build: new Map(), function: new Map(), field: new Map() };
  for (let i = 0; i < lines.length; i++) {
    const f = lines[i].split('\t');
    let kind = null;
    let id = null;
    let what = null;
    if (f[0] === 'build' && f.length >= 5) {
      kind = 'build';
      id = dec(f[1]);
      what = `two build lines for CL-${f[1]}`;
    } else if ((f[0] === 'function' && f.length >= 12) || (f[0] === 'absent' && f.length >= 8)) {
      kind = 'function';
      id = f[1] !== '' && hex(f[2]) !== null && dec(f[3]) !== null ? `${f[1]}\t${hex(f[2])}\t${dec(f[3])}` : null;
      what = `two function or absent rows for ${f[1]} (fp ${f[2]}) on CL-${f[3]}`;
    } else if (f[0] === 'field' && f.length >= 7) {
      kind = 'field';
      id = f[1] !== '' && dec(f[2]) !== null ? `${f[1]}\t${dec(f[2])}` : null;
      what = `two field rows for ${f[1]} on CL-${f[2]}`;
    }
    if (id === null) continue;
    const first = seen[kind].get(id);
    if (first !== undefined) return `${what} (lines ${first} and ${i + 1})`;
    seen[kind].set(id, i + 1);
  }
  return null;
}

export function check(signed, { keys, lastSeq = null, now = Math.floor(Date.now() / 1000), refuseTestKey = false, headerText = null }) {
  need(signed.length > 0 && signed.length <= CAP_BYTES, `the file is ${signed.length} bytes (1 to ${CAP_BYTES} allowed)`);
  need(signed[signed.length - 1] === 0x0a, 'the file does not end in LF');
  const sigStart = signed.lastIndexOf(0x0a, signed.length - 2) + 1;
  const m = /^sig\t([a-z0-9]+)\t([A-Za-z0-9+/]{86}==)$/.exec(signed.subarray(sigStart, signed.length - 1).toString('latin1'));
  need(m, 'the last line is not a sig line');
  const key = Object.hasOwn(keys, m[1]) ? keys[m[1]] : null;
  need(key, `no key in slot ${m[1]}`);
  need(!(refuseTestKey && key.test), `the file is signed with the TEST key in slot ${m[1]}`);
  const body = signed.subarray(0, sigStart);
  const pub = createPublicKey({ key: Buffer.concat([SPKI_PREFIX, Buffer.from(key.hex, 'hex')]), format: 'der', type: 'spki' });
  need(verify(null, body, pub, Buffer.from(m[2], 'base64')), 'the signature does not verify');
  const lines = body.toString('latin1').split('\n');
  need(lines[0] === 'format\t1', 'not a format 1 feed');
  const sm = /^seq\t([1-9][0-9]*)$/.exec(lines[1] ?? '');
  need(sm && Number.isSafeInteger(Number(sm[1])), 'the second line is not a seq line');
  const seq = Number(sm[1]);
  need(seq <= now + MAX_SEQ_AHEAD, `seq ${seq} is more than a day ahead of the clock (${now})`);
  const duplicate = findDuplicate(lines);
  need(duplicate === null, `duplicate entry, the loader would refuse the whole file: ${duplicate}`);
  for (const line of lines) {
    const f = line.split('	');
    if (f[0] === 'function' && f[7] === '-')
      need([f[4], f[5], f[11]].every((v) => /^0x[0-9A-F]+$/.test(v ?? '') && parseInt(v, 16) !== 0),
        `an address-only row for ${f[1]} on CL-${f[3]} needs the exe's stamp and size and an RVA`);
  }
  if (headerText !== null) {
    const have = readHeaderBuilds(headerText);
    for (const line of lines) {
      const f = line.split('\t');
      if (f[0] !== 'build') continue;
      const stamp = parseInt(f[2], 16);
      const size = parseInt(f[3], 16);
      need(have.get(Number(f[1])) === `${stamp} ${size}`, `release gate: the header has no build CL-${f[1]} with the feed's image stamp and size`);
    }
  }
  if (lastSeq !== null && seq <= lastSeq) throw new NotNewer(`seq ${seq} does not rise above ${lastSeq}`);
  return { keyId: m[1], seq, test: key.test };
}

function main(argv) {
  const opt = { flags: new Set() };
  const rest = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--refuse-test-key') opt.flags.add('refuse-test-key');
    else if (['--source', '--last-seq', '--now', '--header'].includes(a) && i + 1 < argv.length) opt[a.slice(2)] = argv[++i];
    else if (a.startsWith('--')) throw new Error(`unknown or incomplete option ${a}`);
    else rest.push(a);
  }
  if (rest.length !== 1 || !opt.source) throw new Error('usage: verify-feed.mjs <signed-feed> --source <version_feed.cpp> [--last-seq n] [--now s] [--refuse-test-key] [--header h]');
  const int = (s, what) => { if (!/^\d+$/.test(s)) throw new Error(`${what} is a non-negative integer`); return Number(s); };
  try {
    const r = check(readFileSync(rest[0]), {
      keys: readKeySlots(readFileSync(opt.source, 'utf8')),
      lastSeq: opt['last-seq'] === undefined ? null : int(opt['last-seq'], '--last-seq'),
      now: opt.now === undefined ? undefined : int(opt.now, '--now'),
      refuseTestKey: opt.flags.has('refuse-test-key'),
      headerText: opt.header ? readFileSync(opt.header, 'utf8') : null,
    });
    process.stdout.write(`${r.seq}\n`);
    process.stderr.write(`verify-feed: ok, key ${r.keyId}${r.test ? ' (TEST key)' : ''}, seq ${r.seq}\n`);
    return EXIT_OK;
  } catch (e) {
    if (e instanceof NotNewer) { process.stderr.write(`verify-feed: not newer: ${e.message}\n`); return EXIT_NOT_NEWER; }
    if (e instanceof Refused) { process.stderr.write(`verify-feed: refused: ${e.message}\n`); return EXIT_REFUSED; }
    throw e;
  }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (e) {
    process.stderr.write(`verify-feed: ${e.message}\n`);
    process.exitCode = EXIT_ERROR;
  }
}
