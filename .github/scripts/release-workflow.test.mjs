// Guards for the resume path of release.yml, read from the workflow text.
//
//   node --test .github/scripts/release-workflow.test.mjs
//
// A resume (resume_tag set) must finish a public release and nothing else: it
// never bumps a version, tags, creates, deletes or replaces anything, and Nexus
// only runs after the release assets were checked.

import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { MANIFEST_ASSET } from './release-check.mjs';

const workflows = join(dirname(fileURLToPath(import.meta.url)), '..', 'workflows');
const text = readFileSync(join(workflows, 'release.yml'), 'utf8').replace(/\r\n/g, '\n');

// { jobName: text of that job } from the top-level `jobs:` block.
const jobs = Object.fromEntries(
  [...text.slice(text.indexOf('\njobs:\n')).matchAll(/\n  ([\w-]+):\n([\s\S]*?)(?=\n  [\w-]+:\n|\n*$)/g)].map((m) => [m[1], m[2]]),
);
const ifOf = (job) => /\n    if: (\|\n(?: {6}.*\n?)+|.*)/.exec(jobs[job])?.[1] ?? '';

test('the jobs are the ones this test knows', () => {
  assert.deepEqual(Object.keys(jobs), ['prepare', 'build', 'release', 'verify-release', 'nexus', 'publish-feed', 'notice', 'sync-plugin-sdk', 'trigger-plugin-builds']);
});

test('a failed Nexus step puts the manual fallback in the run, and the last job names the resume command', () => {
  assert.match(jobs.nexus, /name: Say how to publish Nexus by hand\n {8}if: \$\{\{ failure\(\) \}\}[\s\S]*resume-notice\.mjs nexus-text[\s\S]*::error title=Nexus publish failed::/);
  assert.match(ifOf('notice'), /always\(\) && needs\.prepare\.result == 'success'/);
  assert.match(jobs.notice, /needs: \[prepare, release, verify-release, nexus, publish-feed\]/);
  assert.match(jobs.notice, /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" >/);
  assert.match(jobs.notice, /resume-notice\.mjs release/);
  assert.doesNotMatch(text, /issues: write|gh issue/);
});

test('only the build and the release job are skipped on a resume', () => {
  assert.match(ifOf('build'), /resume_tag == ''/);
  assert.match(ifOf('release'), /resume_tag == ''/);
  for (const job of ['verify-release', 'publish-feed']) {
    assert.doesNotMatch(ifOf(job), /resume_tag == ''/, `${job} must run on a resume`);
  }
  // Nexus names resume_tag only to apply resume_nexus on a resume; a fresh run always runs it.
  assert.doesNotMatch(ifOf('nexus'), /&&\s*github\.event\.inputs\.resume_tag == ''/);
});

test('a resume computes no version and finds its release first', () => {
  const prepare = jobs.prepare;
  assert.match(prepare, /name: Generate release tag\n {8}id: tag\n {8}if: \$\{\{ github.event.inputs.resume_tag == '' \}\}/);
  assert.match(prepare, /name: Find the release to resume\n {8}id: resume\n {8}if: \$\{\{ github.event.inputs.resume_tag != '' \}\}/);
  assert.match(prepare, /releases\/tags\/\$RESUME_TAG/);
  assert.match(prepare, /nothing to resume/);
});

test('a tag that already exists stops a new run instead of bumping or reusing it', () => {
  assert.match(jobs.prepare, /git ls-remote --exit-code --tags origin "refs\/tags\/\$tag"[\s\S]*resume_tag=\$tag/);
});

test('jobs that run on a resume never tag, create, replace, delete or force', () => {
  const forbidden = /gh release (create|delete|edit|upload)|git (tag|push)|--clobber|--force|--cleanup-tag|gh api -X (DELETE|PATCH|PUT|POST)/;
  for (const job of ['prepare', 'verify-release', 'nexus']) {
    assert.doesNotMatch(jobs[job], forbidden, `${job} must not change the release`);
  }
});

test('Nexus waits for the asset check, which does not ask for the version map', () => {
  assert.match(jobs.nexus, /needs: \[prepare, release, verify-release\]/);
  assert.match(ifOf('nexus'), /needs\.verify-release\.result == 'success'/);
  assert.match(jobs['verify-release'], /release-check\.mjs --repo "\$GITHUB_REPOSITORY" --tag "\$TAG" --version-map skip/);
  assert.match(jobs.nexus, /gh release download \$env:TAG/, 'Nexus takes the files that were checked');
  assert.doesNotMatch(text, /modloader-zips/);
});

test('the resume options reach Nexus', () => {
  assert.match(jobs.nexus, /name: Upload Client build to Nexus Mods\n {8}if: \$\{\{ github.event.inputs.resume_tag == '' \|\| github.event.inputs.resume_nexus != 'server-only' \}\}/);
  assert.match(ifOf('nexus'), /\(github\.event\.inputs\.resume_tag == '' \|\| github\.event\.inputs\.resume_nexus != 'none'\)/, 'a fresh run ignores resume_nexus');
});

test('a pwsh step must not end on a native exit code that is not a failure', () => {
  // The runner ends a pwsh step with $LASTEXITCODE: ls-remote exits 2 for "no such tag"
  // and git show exits 128 for "no such file", both normal here.
  assert.match(jobs.prepare, /git ls-remote --exit-code --tags origin "refs\/tags\/\$tag" \*> \$null\n.*\n.*\n.*\n {10}\$global:LASTEXITCODE = 0\n/);
  assert.match(jobs.nexus, /git show "refs\/tags\/[^\n]*2>\$null[^\n]*\n.*\n {10}\$global:LASTEXITCODE = 0\n/);
});

test('the manifest the release uploads has the name the check reads', () => {
  assert.ok(jobs.release.includes(`"$ws\\${MANIFEST_ASSET}"`));
  assert.match(jobs.release, /release-check\.mjs" write-manifest --tag \$tag --out \$manifest @assets\n.*\n {10}\$assets\.Add\(\$manifest\)/);
});

test('one release run at a time', () => {
  assert.match(text, /\nconcurrency:\n {2}group: release\n {2}cancel-in-progress: false\n/);
});
