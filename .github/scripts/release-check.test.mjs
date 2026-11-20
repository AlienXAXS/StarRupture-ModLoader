// Tests for release-check.mjs. `gh` is replaced by a fake, so nothing here
// touches GitHub.
//
//   node --test .github/scripts/release-check.test.mjs

import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { checkRelease, ghIo, main, MANIFEST_ASSET, VERSION_MAP_ASSET } from './release-check.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const REPO = 'o/r';
const TAG = 'v1.2.3';
const md5 = (b) => createHash('md5').update(b).digest('hex');
const ZIP = Buffer.from('zip bytes');
const MAP = Buffer.from('format\t1\nseq\t7\nbuild\t130045\t0x1\t0x2\tpublic\nsig\tk2\tAAAA\n');

// A release whose assets are `files` ({ name: Buffer }); the manifest lists `listed`.
function fixture({ files = { 'a.zip': ZIP }, listed = Object.keys(files), manifestTag = TAG, extra = {}, latest = TAG, prerelease = false, noManifest = false } = {}) {
  const blobs = { ...files };
  const entries = listed.map((n) => ({ name: n, size: files[n] ? files[n].length : 1, md5: files[n] ? md5(files[n]) : md5(Buffer.from('x')) }));
  if (!noManifest) blobs[MANIFEST_ASSET] = Buffer.from(JSON.stringify({ tag: manifestTag, assets: entries }));
  const assets = Object.entries({ ...blobs, ...extra }).map(([name, body], i) => ({ id: i + 1, name, size: body.length, state: 'uploaded' }));
  const store = new Map(assets.map((a) => [a.id, { ...blobs, ...extra }[a.name]]));
  const calls = [];
  return {
    assets, calls,
    io: {
      release: () => ({ prerelease, assets }),
      latestTag: () => latest,
      download: (_r, a) => { calls.push(a.name); return store.get(a.id); },
    },
  };
}
const codes = (r) => r.problems.map((p) => p.code);

test('a whole release is complete and a bare repo/tag call is enough', () => {
  const f = fixture({ extra: { [VERSION_MAP_ASSET]: MAP } });
  const r = checkRelease({ repo: REPO, tag: TAG }, f.io);
  assert.equal(r.complete, true);
  assert.equal(r.expected, 1);
  assert.deepEqual(r.versionMap, { seq: 7 });
});

test('no published release for the tag', () => {
  const r = checkRelease({ repo: REPO, tag: TAG }, { release: () => null });
  assert.deepEqual(codes(r), ['release_missing']);
  assert.equal(r.complete, false);
});

test('an asset the manifest lists but the release lacks', () => {
  const f = fixture({ files: { 'a.zip': ZIP }, listed: ['a.zip', 'b.zip'], extra: { [VERSION_MAP_ASSET]: MAP } });
  const r = checkRelease({ repo: REPO, tag: TAG }, f.io);
  assert.deepEqual(r.problems, [{ code: 'asset_missing', name: 'b.zip' }]);
});

test('a wrong md5 with the right size is caught by the download', () => {
  const f = fixture({ files: { 'a.zip': ZIP } });
  const good = f.assets.find((a) => a.name === 'a.zip');
  const bad = Buffer.from('zip bytez');           // same length, other bytes
  const io = { ...f.io, download: (_r, a) => (a.id === good.id ? bad : f.io.download(_r, a)) };
  const r = checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, io);
  assert.deepEqual(codes(r), ['md5_mismatch']);
  assert.equal(r.problems[0].expected, md5(ZIP));
  assert.equal(r.problems[0].actual, md5(bad));
});

test('a wrong size is reported without downloading the asset', () => {
  const f = fixture({ files: { 'a.zip': ZIP } });
  f.assets.find((a) => a.name === 'a.zip').size = 3;
  const r = checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, f.io);
  assert.deepEqual(codes(r), ['size_mismatch']);
  assert.deepEqual(f.calls, [MANIFEST_ASSET]);
});

test('an asset that is not fully uploaded', () => {
  const f = fixture({ files: { 'a.zip': ZIP } });
  f.assets.find((a) => a.name === 'a.zip').state = 'starter';
  const r = checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, f.io);
  assert.deepEqual(codes(r), ['asset_not_uploaded']);
});

test('a release with no manifest is incomplete, not trusted', () => {
  const r = checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, fixture({ noManifest: true }).io);
  assert.deepEqual(codes(r), ['manifest_missing']);
});

test('a manifest for another tag or in a bad shape is refused', () => {
  assert.deepEqual(codes(checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, fixture({ manifestTag: 'v9.9.9' }).io)), ['manifest_invalid']);
  const f = fixture();
  const m = f.assets.find((a) => a.name === MANIFEST_ASSET);
  const io = { ...f.io, download: (_r, a) => (a.id === m.id ? Buffer.from('{"tag":"v1.2.3","assets":[{"name":"a.zip"}]}') : f.io.download(_r, a)) };
  assert.deepEqual(codes(checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, io)), ['manifest_invalid']);
});

test('the version map is required on the Latest release only (auto)', () => {
  assert.deepEqual(codes(checkRelease({ repo: REPO, tag: TAG }, fixture().io)), ['version_map_missing']);
  assert.equal(checkRelease({ repo: REPO, tag: TAG }, fixture({ latest: 'v2.0.0' }).io).complete, true, 'an older release needs none');
  assert.equal(checkRelease({ repo: REPO, tag: TAG }, fixture({ prerelease: true }).io).complete, true, 'a preview needs none');
});

test('skip never asks for it and require always does', () => {
  assert.equal(checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, fixture().io).complete, true);
  assert.deepEqual(codes(checkRelease({ repo: REPO, tag: TAG, versionMap: 'require' }, fixture({ latest: 'v2.0.0' }).io)), ['version_map_missing']);
});

test('a version map that is not a signed format 1 file', () => {
  for (const body of ['', 'format\t1\nseq\t7\nbuild\t1\t2\t3\tx\n', 'format\t2\nseq\t7\nsig\tk2\tAAAA\n', 'format\t1\nseq\tx\nsig\tk2\tAAAA\n']) {
    const r = checkRelease({ repo: REPO, tag: TAG }, fixture({ extra: { [VERSION_MAP_ASSET]: Buffer.from(body) } }).io);
    assert.deepEqual(codes(r), ['version_map_invalid'], JSON.stringify(body));
  }
});

test('--cl needs a build row for that game build, and implies require', () => {
  const have = fixture({ extra: { [VERSION_MAP_ASSET]: MAP }, latest: 'v2.0.0' });
  assert.equal(checkRelease({ repo: REPO, tag: TAG, cl: '130045' }, have.io).complete, true);
  assert.deepEqual(codes(checkRelease({ repo: REPO, tag: TAG, cl: '130046' }, have.io)), ['version_map_cl_missing']);
  assert.deepEqual(codes(checkRelease({ repo: REPO, tag: TAG, cl: '130045' }, fixture().io)), ['version_map_missing']);
});

test('main prints one JSON object and maps the result to an exit code', () => {
  const lines = [];
  const log = console.log;
  console.log = (s) => lines.push(s);
  try {
    const ok = fixture({ extra: { [VERSION_MAP_ASSET]: MAP } });
    assert.equal(main(['--repo', REPO, '--tag', TAG], ok.io), 0);
    assert.equal(main(['--repo', REPO, '--tag', TAG], fixture().io), 1);
    assert.equal(main(['--repo', REPO], ok.io), 2);
    assert.equal(main(['--repo', REPO, '--tag', TAG, '--version-map', 'maybe'], ok.io), 2);
    assert.equal(main(['--repo', REPO, '--tag', TAG], { release: () => { throw new Error('gh failed: boom'); } }), 2);
  } finally { console.log = log; }
  const parsed = lines.map((l) => JSON.parse(l));
  assert.equal(parsed[0].complete, true);
  assert.deepEqual(parsed[1].problems, [{ code: 'version_map_missing', name: VERSION_MAP_ASSET }]);
  assert.match(parsed[2].error, /--tag/);
  assert.match(parsed[4].error, /boom/);
});

test('ghIo: a 404 is an answer, any other failure is an error, downloads use the asset id', () => {
  const seen = [];
  const run = (reply) => (args) => { seen.push(args); return reply; };
  const notFound = ghIo(run({ status: 1, stderr: Buffer.from('gh: Not Found (HTTP 404)') }));
  assert.equal(notFound.release(REPO, TAG), null);
  assert.equal(notFound.latestTag(REPO), null);
  assert.deepEqual(seen[0], ['api', `repos/${REPO}/releases/tags/${TAG}`]);
  assert.throws(() => ghIo(run({ status: 1, stderr: Buffer.from('HTTP 502') })).release(REPO, TAG), /502/);
  assert.throws(() => ghIo(run({ error: new Error('ENOENT') })).release(REPO, TAG), /could not run/);
  const ok = ghIo(run({ status: 0, stdout: Buffer.from('bytes') }));
  assert.equal(String(ok.download(REPO, { id: 42, name: 'a.zip' })), 'bytes');
  assert.equal(seen.at(-1).at(-1), `repos/${REPO}/releases/assets/42`);
});

test('write-manifest lists the files it is given, and the check accepts what it wrote', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rel-'));
  const note = Buffer.from('{"build_tag":"v1.2.3"}');
  writeFileSync(join(dir, 'a.zip'), ZIP);
  writeFileSync(join(dir, 'manifest-client.json'), note);
  const out = join(dir, MANIFEST_ASSET);
  const log = console.log;
  console.log = () => {};
  try {
    assert.equal(main(['write-manifest', '--tag', TAG, '--out', out, join(dir, 'a.zip'), join(dir, 'manifest-client.json')]), 0);
    assert.equal(main(['write-manifest', '--tag', TAG, '--out', out]), 2, 'no files is a usage error');
  } finally { console.log = log; }
  const written = JSON.parse(readFileSync(out, 'utf8'));
  assert.deepEqual(written, { tag: TAG, assets: [
    { name: 'a.zip', size: ZIP.length, md5: md5(ZIP) },
    { name: 'manifest-client.json', size: note.length, md5: md5(note) },
  ] });
  const f = fixture({ files: { 'a.zip': ZIP, 'manifest-client.json': note } });
  assert.equal(checkRelease({ repo: REPO, tag: TAG, versionMap: 'skip' }, f.io).complete, true);
});

test('the asset names here are the ones the workflows use', () => {
  assert.match(readFileSync(join(root, '.github', 'workflows', 'vmap-publish.yml'), 'utf8'), new RegExp(`FEED_ASSET: ${VERSION_MAP_ASSET.replace(/\./g, '\\.')}`));
});
