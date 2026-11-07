// Tests for check-public-builds.mjs.
//
//   node --test .github/vmap/check-public-builds.test.mjs

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { check, readPublicBuilds } from './check-public-builds.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const TOOL = join(here, 'check-public-builds.mjs');

const PUBLIC = '{ 127004, 0xE8972298, 0x0F35C000, "public 25646882" },';
const TEST = '{ 128786, 0xCF8579A6, 0x0F47D000, "steam_build 26271156" },';
const header = (builds, rows) => `inline constexpr uint32_t kBuiltCL = 127004;
inline constexpr std::array<Build, 2> kBuilds = {{
	${builds.join('\n\t')}
}};
inline constexpr std::array<FieldRow, 2> kFieldRows = {{
	${rows.join('\n\t')}
}};
`;
const row = (cl) => `{ ${cl}, 0x10, 4, 0, "ACrFoo::bBar" },`;
const LIST_JSON = JSON.stringify([{ cl: 127004, buildid: '25646882', firstSeen: '2026-01-01' }]);
const publicBuilds = readPublicBuilds(LIST_JSON);

test('a header with only public builds passes', () => {
  assert.deepEqual(check(publicBuilds, [['h', header([PUBLIC], [row(127004)])]]), []);
});

test('an extra test-only build in the table fails and is named', () => {
  const p = check(publicBuilds, [['h', header([PUBLIC, TEST], [row(127004)])]]);
  assert.equal(p.length, 1);
  assert.match(p[0], /CL-128786 is not a public build/);
});

test('a row of a test-only CL fails even without a build entry', () => {
  const p = check(publicBuilds, [['h', header([PUBLIC], [row(127004), row(128786)])]]);
  assert.match(p.join('\n'), /CL-128786/);
});

test('a public CL with another Steam build id fails', () => {
  const other = '{ 127004, 0xE8972298, 0x0F35C000, "public 99999999" },';
  assert.match(check(publicBuilds, [['h', header([other], [])]]).join('\n'), /public build is 25646882/);
});

test('the command exits 0 on public-only and 3 on a test-only CL', () => {
  const dir = mkdtempSync(join(tmpdir(), 'pub-'));
  const good = join(dir, 'good.h');
  const bad = join(dir, 'bad.h');
  const list = join(dir, 'public-builds.json');
  writeFileSync(list, LIST_JSON);
  writeFileSync(good, header([PUBLIC], [row(127004)]));
  writeFileSync(bad, header([PUBLIC, TEST], [row(127004), row(128786)]));
  assert.equal(spawnSync(process.execPath, [TOOL, '--public', list, good], { windowsHide: true }).status, 0);
  const r = spawnSync(process.execPath, [TOOL, '--public', list, bad], { encoding: 'utf8', windowsHide: true });
  assert.equal(r.status, 3);
  assert.match(r.stderr, /CL-128786 is not a public build/);
});

test('an unreadable list fails', () => {
  const dir = mkdtempSync(join(tmpdir(), 'pub-'));
  const good = join(dir, 'good.h');
  writeFileSync(good, header([PUBLIC], []));
  const r = spawnSync(process.execPath, [TOOL, '--public', join(dir, 'missing.json'), good], { encoding: 'utf8', windowsHide: true });
  assert.equal(r.status, 3);
  assert.match(r.stderr, /Cannot read the list of public builds/);
});
