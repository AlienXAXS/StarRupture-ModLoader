// Tests for resume-notice.mjs and for how vmap-publish.yml uses it.
//
//   node --test .github/scripts/resume-notice.test.mjs

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { main, notice, vmapCommand } from './resume-notice.mjs';

const workflow = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'workflows', 'vmap-publish.yml'), 'utf8').replace(/\r\n/g, '\n');
const ENV = {
  GITHUB_REPOSITORY: 'o/r', GITHUB_SERVER_URL: 'https://github.com', GITHUB_RUN_ID: '42', GITHUB_REF_NAME: 'main',
  FEED_REPO: 'x/ci', FEED_RELEASE: 'vmap-feed', ALLOW_TEST_KEY: 'false', FIRST_PUBLISH: 'false',
  NEEDS_JSON: JSON.stringify({ publish: { result: 'failure', outputs: {} } }),
};

test('a failed run names the one command with the exact inputs', () => {
  const text = notice('vmap', ENV);
  assert.match(text, /publish \(failure\) in https:\/\/github\.com\/o\/r\/actions\/runs\/42/);
  assert.ok(text.includes('```\ngh workflow run vmap-publish.yml --repo o/r --ref main -f feed_repo=x/ci -f feed_release=vmap-feed\n```'));
  assert.equal(text.split('gh workflow run').length, 2, 'exactly one command');
});

test('a cancelled run is named too, and the boolean inputs are kept when set', () => {
  const env = { ...ENV, NEEDS_JSON: JSON.stringify({ publish: { result: 'cancelled' } }), FIRST_PUBLISH: 'true', ALLOW_TEST_KEY: 'true' };
  assert.match(notice('vmap', env), /publish \(cancelled\)/);
  assert.equal(vmapCommand(env), 'gh workflow run vmap-publish.yml --repo o/r --ref main -f feed_repo=x/ci -f feed_release=vmap-feed -f allow_test_key=true -f first_publish=true');
});

test('nothing failed, nothing printed', () => {
  const env = { ...ENV, NEEDS_JSON: JSON.stringify({ publish: { result: 'success' } }) };
  assert.equal(notice('vmap', env), '');
});

test('main: exit 1 when stuck, 0 when not, 2 for a bad call', () => {
  const log = console.log;
  const err = console.error;
  console.log = () => {};
  console.error = () => {};
  try {
    assert.equal(main(['vmap'], ENV), 1);
    assert.equal(main(['vmap'], { ...ENV, NEEDS_JSON: '{}' }), 0);
    assert.equal(main(['other'], ENV), 2);
  } finally { console.log = log; console.error = err; }
});

test('the workflow opens no issue, takes no issue permission and ends in the notice job', () => {
  assert.doesNotMatch(workflow, /issues: write|gh issue|gh api -X POST/);
  assert.match(workflow, /\n {2}notice:\n {4}needs: publish\n {4}if: \$\{\{ always\(\) && \(needs\.publish\.result == 'failure' \|\| needs\.publish\.result == 'cancelled'\) \}\}/);
  assert.match(workflow, /node \.github\/scripts\/resume-notice\.mjs vmap/);
  assert.match(workflow, /NEEDS_JSON: \$\{\{ toJSON\(needs\) \}\}/);
});

// The missing-asset step, run for real with a fake `gh` that lists `assets`.
function runMissingAssetStep(assets, ghFails = false) {
  const step = /- name: Check the signed version map is there\n {8}run: \|\n((?: {10}.*\n)+)/.exec(workflow)[1].replace(/^ {10}/gm, '');
  const dir = mkdtempSync(join(tmpdir(), 'vm-'));
  const fake = join(dir, 'gh');
  writeFileSync(fake, ghFails ? '#!/bin/bash\nexit 1\n' : `#!/bin/bash\nprintf '%s\\n' ${assets.map((a) => `'${a}'`).join(' ')}\n`);
  chmodSync(fake, 0o755);
  return spawnSync('bash', ['-c', step], {
    encoding: 'utf8',
    windowsHide: true,
    env: { ...process.env, PATH: `${dir}${delimiter}${process.env.PATH}`, FEED_RELEASE: 'vmap-feed', FEED_REPO: 'x/ci', FEED_ASSET: 'version-map-client.v1.txt', GITHUB_REPOSITORY: 'o/r' },
  });
}

test('a signed version map that is not on the release fails with a clear message', () => {
  const missing = runMissingAssetStep(['other.txt', 'version-map-client.v1.txt.bak']);
  assert.equal(missing.status, 1);
  assert.match(missing.stdout, /::error title=No signed version map to copy::Release vmap-feed in x\/ci has no version-map-client\.v1\.txt/);
  const unreadable = runMissingAssetStep([], true);
  assert.equal(unreadable.status, 1);
  assert.match(unreadable.stdout, /Signed version map not readable/);
  const there = runMissingAssetStep(['a', 'version-map-client.v1.txt']);
  assert.equal(there.status, 0);
});
