#!/usr/bin/env node
// Release gate: a release carries version-map rows only for public game builds.
//
//   node check-public-builds.mjs [--public <file-or-url>] <header>...
//
// The list of public builds is state/public-builds.json in the starrupture-ci
// repo: a JSON array of { cl, buildid, firstSeen }, one entry per build that
// has been on the public Steam branch. It is fetched from PUBLIC_BUILDS_URL
// unless --public gives a local file (or another URL).
// Every header given (version_map.generated.h, layout_table.generated.h) is
// read for the game CLs it names: the build table, every row's CL and kBuiltCL.
// A CL that is not in the list, or a build whose `source` text names another
// Steam build id than the listed one, fails the check. A list that cannot be
// read fails it too.
//
// Exit codes: 0 all public; 3 a build is not public or the list is unreadable;
// 2 usage error.

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export const PUBLIC_BUILDS_URL = 'https://raw.githubusercontent.com/msantoro12/starrupture-ci/main/state/public-builds.json';

export const EXIT_OK = 0;
export const EXIT_ERROR = 2;
export const EXIT_NOT_PUBLIC = 3;

// { cl -> build id } from the list's JSON text.
export function readPublicBuilds(text) {
  const list = JSON.parse(text);
  if (!Array.isArray(list)) throw new Error('the public builds list is not a JSON array');
  const out = new Map();
  for (const e of list) {
    if (!Number.isInteger(Number(e?.cl)) || e?.buildid === undefined) throw new Error('a public builds entry needs cl and buildid');
    out.set(Number(e.cl), String(e.buildid));
  }
  return out;
}

async function loadList(where) {
  if (!/^https?:\/\//.test(where)) return readFileSync(where, 'utf8');
  const res = await fetch(where);
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${where}`);
  return res.text();
}

// Every game CL a header carries, and the Steam build id of each build-table entry.
export function readHeaderCLs(headerText) {
  const cls = new Set();
  const ids = new Map();
  for (const m of headerText.matchAll(/kBuiltCL\s*=\s*(\d+)/g)) cls.add(Number(m[1]));
  const block = /kBuilds\s*=\s*\{\{([\s\S]*?)\}\};/.exec(headerText);
  if (block) {
    // The version map: every row starts with its game CL.
    for (const m of headerText.matchAll(/^\s*\{\s*(\d+),/gm)) cls.add(Number(m[1]));
    for (const m of block[1].matchAll(/\{\s*(\d+),[^"]*"[^"]*?(\d+)"\s*\}/g)) ids.set(Number(m[1]), m[2]);
  }
  return { cls, ids };
}

// Returns the list of problems, empty when every CL is public.
export function check(publicBuilds, headers) {
  const problems = [];
  for (const [name, text] of headers) {
    const { cls, ids } = readHeaderCLs(text);
    for (const cl of [...cls].sort((a, b) => a - b)) {
      if (!publicBuilds.has(cl)) problems.push(`${name}: CL-${cl} is not a public build`);
      else if (ids.has(cl) && ids.get(cl) !== publicBuilds.get(cl)) {
        problems.push(`${name}: CL-${cl} names Steam build ${ids.get(cl)}, the public build is ${publicBuilds.get(cl)}`);
      }
    }
  }
  return problems;
}

async function main(argv) {
  let where = PUBLIC_BUILDS_URL;
  const files = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--public' && i + 1 < argv.length) where = argv[++i];
    else files.push(argv[i]);
  }
  if (!files.length) {
    console.error('usage: check-public-builds.mjs [--public <file-or-url>] <header>...');
    return EXIT_ERROR;
  }
  let publicBuilds;
  try {
    publicBuilds = readPublicBuilds(await loadList(where));
  } catch (e) {
    console.error(`::error::Cannot read the list of public builds (${where}): ${e.message}`);
    return EXIT_NOT_PUBLIC;
  }
  const problems = check(publicBuilds, files.map((f) => [f, readFileSync(f, 'utf8')]));
  if (!problems.length) return EXIT_OK;
  for (const p of problems) console.error(`::error::${p}`);
  console.error('A release carries version-map rows only for builds on the public Steam branch. Remove the rows of the CL(s) above from the version map.');
  return EXIT_NOT_PUBLIC;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) process.exit(await main(process.argv.slice(2)));
