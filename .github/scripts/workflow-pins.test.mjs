// Every action used by a workflow that can write to the repository is pinned to a
// commit sha, with the version it was resolved from in a trailing comment, so a
// moved tag cannot change what a release or a version map copy runs.
//
//   node --test .github/scripts/workflow-pins.test.mjs
//
// release.yml and vmap-publish.yml are always checked; so is any other workflow
// that asks for `contents: write`. Local calls (`uses: ./...`) have no tag to move.

import test from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = join(dirname(fileURLToPath(import.meta.url)), '..', 'workflows');
const read = (f) => readFileSync(join(dir, f), 'utf8').replace(/\r\n/g, '\n');
const checked = readdirSync(dir).filter((f) => /\.ya?ml$/.test(f) && (['release.yml', 'vmap-publish.yml'].includes(f) || /contents: write/.test(read(f))));
const usesLines = (f) => read(f).split('\n').filter((l) => /^\s*(- )?uses: /.test(l)).map((l) => l.replace(/^\s*(- )?uses: /, ''));

test('the workflows that write are the ones this test knows', () => {
  assert.deepEqual(checked, ['cleanup_tags.yml', 'draft.yml', 'prune_releases.yml', 'release.yml', 'vmap-publish.yml']);
});

test('every action in them is pinned to a commit sha with its version in a comment', () => {
  let remote = 0;
  for (const f of checked) {
    for (const uses of usesLines(f)) {
      if (uses.startsWith('./')) continue;
      remote++;
      assert.match(uses, /^[\w.-]+\/[\w.-]+(\/[\w./-]+)?@[0-9a-f]{40} # \S+$/, `${f}: ${uses}`);
    }
  }
  assert.ok(remote >= 20, 'the test saw the actions');
});

test('one version of an action is used everywhere, so the shas agree', () => {
  const seen = new Map();
  for (const f of checked) {
    for (const uses of usesLines(f)) {
      if (uses.startsWith('./')) continue;
      const [name, rest] = uses.split('@');
      assert.equal(seen.get(name) ?? rest, rest, `${f}: ${name}`);
      seen.set(name, rest);
    }
  }
});
