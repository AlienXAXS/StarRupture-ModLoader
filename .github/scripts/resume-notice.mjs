#!/usr/bin/env node
// Writes the text a red run shows for a step that did not finish: what is stuck
// and the one command that finishes it. vmap-publish.yml calls it from its last
// job and puts the text in the job summary; the run's failure e-mail is the
// alert. Nothing is opened, posted or changed anywhere.
//
//   node resume-notice.mjs vmap
//
// Reads from the environment: NEEDS_JSON (${{ toJSON(needs) }} of the calling
// job), GITHUB_REPOSITORY, GITHUB_SERVER_URL, GITHUB_RUN_ID, GITHUB_REF_NAME,
// and the vmap-publish inputs FEED_REPO, FEED_RELEASE, ALLOW_TEST_KEY,
// FIRST_PUBLISH.
//
// Prints markdown on stdout. Exit 0 when no job failed or was cancelled (prints
// nothing), 1 when something is stuck, 2 for a bad call.

import { pathToFileURL } from 'node:url';

const BAD = new Set(['failure', 'cancelled']);
const fence = (cmd) => `\`\`\`\n${cmd}\n\`\`\``;
const runUrl = (env) => `${env.GITHUB_SERVER_URL}/${env.GITHUB_REPOSITORY}/actions/runs/${env.GITHUB_RUN_ID}`;
const failedJobs = (env) => Object.entries(JSON.parse(env.NEEDS_JSON || '{}')).filter(([, n]) => BAD.has(n.result)).map(([name, n]) => `${name} (${n.result})`);

// The one command that runs vmap-publish.yml again with the inputs of this run.
export function vmapCommand(env) {
  const flags = [`-f feed_repo=${env.FEED_REPO}`, `-f feed_release=${env.FEED_RELEASE}`];
  if (env.ALLOW_TEST_KEY === 'true') flags.push('-f allow_test_key=true');
  if (env.FIRST_PUBLISH === 'true') flags.push('-f first_publish=true');
  return `gh workflow run vmap-publish.yml --repo ${env.GITHUB_REPOSITORY} --ref ${env.GITHUB_REF_NAME} ${flags.join(' ')}`;
}

// The markdown for a kind, or '' when nothing failed.
export function notice(kind, env) {
  const failed = failedJobs(env);
  if (failed.length === 0) return '';
  return [
    '## The version object map was not copied to the Latest release',
    '',
    `${failed.join(', ')} in ${runUrl(env)}. Players keep the version map they have until a run copies it.`,
    '',
    `It is copied from ${env.FEED_REPO}, release ${env.FEED_RELEASE}. Run this one command to copy it again; it changes nothing when Latest already has the newest version map:`,
    '',
    fence(vmapCommand(env)),
    '',
  ].join('\n');
}

export function main(argv, env = process.env) {
  if (argv[0] !== 'vmap') {
    console.error('usage: resume-notice.mjs vmap');
    return 2;
  }
  const text = notice(argv[0], env);
  if (text) console.log(text);
  return text ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exitCode = main(process.argv.slice(2));
}
