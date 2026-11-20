#!/usr/bin/env node
// Writes the text a red run shows for a step that did not finish: what is stuck
// and the one command that finishes it. vmap-publish.yml and release.yml call it
// from their last job and put the text in the job summary; the run's failure
// e-mail is the alert. Nothing is opened, posted or changed anywhere.
//
//   node resume-notice.mjs vmap|release     the notice for the run
//   node resume-notice.mjs nexus-text       what to do by hand when Nexus fails
//
// Reads from the environment: NEEDS_JSON (${{ toJSON(needs) }} of the calling
// job), GITHUB_REPOSITORY, GITHUB_SERVER_URL, GITHUB_RUN_ID, GITHUB_REF_NAME.
//   vmap:    FEED_REPO, FEED_RELEASE, FIRST_PUBLISH (the
//            vmap-publish inputs)
//   release: TAG, CHECK_JSON (the output of release-check.mjs) and FIND_JSON (the
//            output of `release-check.mjs find`: is the tag a draft, public or
//            neither)
//
// Prints markdown on stdout. Exit 0 when nothing is stuck (prints nothing), 1
// when something is, 2 for a bad call.

import { pathToFileURL } from 'node:url';

const BAD = new Set(['failure', 'cancelled']);
const fence = (cmd) => `\`\`\`\n${cmd}\n\`\`\``;
const runUrl = (env) => `${env.GITHUB_SERVER_URL}/${env.GITHUB_REPOSITORY}/actions/runs/${env.GITHUB_RUN_ID}`;
const failedJobs = (env) => Object.entries(JSON.parse(env.NEEDS_JSON || '{}')).filter(([, n]) => BAD.has(n.result)).map(([name, n]) => `${name} (${n.result})`);

// The one command that runs vmap-publish.yml again with the inputs of this run.
export function vmapCommand(env) {
  const flags = [`-f feed_repo=${env.FEED_REPO}`, `-f feed_release=${env.FEED_RELEASE}`];
  if (env.FIRST_PUBLISH === 'true') flags.push('-f first_publish=true');
  return `gh workflow run vmap-publish.yml --repo ${env.GITHUB_REPOSITORY} --ref ${env.GITHUB_REF_NAME} ${flags.join(' ')}`;
}

// The one command that finishes a release that is already public. Nexus is
// skipped (`nexus: false`) when it is not what failed: a repeat would add a
// second changelog entry there.
export function resumeCommand(env, { nexus = true } = {}) {
  return `gh workflow run release.yml --repo ${env.GITHUB_REPOSITORY} --ref ${env.GITHUB_REF_NAME} -f resume_tag=${env.TAG}${nexus ? '' : ' -f resume_nexus=none'}`;
}

// A release that is still a draft is stuck: the one command repairs it if it needs
// it and publishes it. Deleting a draft is the owner's call and no command here does it.
function draftNotice(env, failed, find) {
  const lines = [`## Release ${env.TAG} is a draft and was not published`, '', `${failed.length ? failed.join(', ') : 'The run ended before the publish'} in ${runUrl(env)}. Nothing is public.`, ''];
  if (find.state === 'ambiguous') {
    lines.push(`More than one draft has the tag ${env.TAG}, so it is not clear which one to resume. Deleting the extra drafts by hand is the owner's call; nothing deletes them for you.`);
  } else {
    lines.push(
      `Draft ${env.TAG} exists; re-run the one command below to repair and publish it, or delete the draft by hand (the owner's call). A draft with a missing or wrong asset is rebuilt whole from its commit; a whole one is checked and published. It never makes a new release, tag or version.`,
      '',
      fence(resumeCommand(env)),
    );
  }
  return `${lines.join('\n')}\n`;
}

// What to do by hand when the Nexus upload fails; the lead publishes Nexus then.
export function nexusText(env) {
  const { GITHUB_REPOSITORY: repo, TAG: tag, GITHUB_REF_NAME: ref } = env;
  return [
    `The Nexus upload for ${tag} did not finish. The GitHub release is public and its assets passed the check.`,
    'Run the one command below, or publish Nexus by hand:',
    `- Command (uploads from the release and never makes a new one; add "-f resume_nexus=server-only" if the Client file is already on Nexus): ${resumeCommand(env)}`,
    `- Files: StarRupture-ModLoader-Client-${tag}.zip (Client file group) and StarRupture-ModLoader-Server-${tag}.zip (Server file group), both on https://github.com/${repo}/releases/tag/${tag}`,
    `- Page text: nexus/description.bbcode on the ref the run was dispatched from (${ref}), with {{TAG}} set to ${tag} and {{REPO}} to ${repo}; page version ${tag}; changelog is UPDATE_NOTES.txt as it was at ${tag}.`,
  ].join('\n');
}

function vmapNotice(env, failed) {
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

function releaseNotice(env, failed) {
  const find = JSON.parse(env.FIND_JSON || '{}');
  if (find.state === 'draft' || find.state === 'ambiguous') return draftNotice(env, failed, find);
  const check = JSON.parse(env.CHECK_JSON || '{}');
  const problems = check.complete ? [] : (check.problems ?? [{ code: 'check_failed', name: check.error }]);
  // A tag with no public release: nothing is stuck, the next run retries.
  if (problems.some((p) => p.code === 'release_missing')) return '';
  if (failed.length === 0 && problems.length === 0) return '';

  const found = [...failed, ...problems.map((p) => (p.name ? `${p.code} ${p.name}` : p.code))].join(', ');
  const lines = [`## Release ${env.TAG} is public but not finished`, '', `${found} in ${runUrl(env)}.`, ''];
  if (problems.some((p) => /^(asset_|size_|md5_|manifest_)/.test(p.code))) {
    lines.push(
      'Repair refused: the release is public and is missing assets or holds a wrong file. A public release is never repaired in place, because its assets are what players download (the loader\'s update manifest URL points at the Latest release), so no command fixes it. Pulling this release by hand (the release and its tag) and running release.yml again is the owner\'s call; nothing deletes it for you.',
      '',
      `release-check.mjs said: \`${JSON.stringify(check)}\``,
    );
  } else {
    const nexusFailed = failed.some((f) => f.startsWith('nexus '));
    lines.push('Run this one command to finish it. It never makes a new release, tag or version:', '', fence(resumeCommand(env, { nexus: nexusFailed })));
    if (nexusFailed) lines.push('', nexusText(env));
  }
  return `${lines.join('\n')}\n`;
}

// The markdown for a kind, or '' when nothing is stuck.
export function notice(kind, env) {
  const failed = failedJobs(env);
  if (kind === 'vmap') return failed.length === 0 ? '' : vmapNotice(env, failed);
  return releaseNotice(env, failed);
}

export function main(argv, env = process.env) {
  if (argv[0] === 'nexus-text') {
    console.log(nexusText(env));
    return 0;
  }
  if (argv[0] !== 'vmap' && argv[0] !== 'release') {
    console.error('usage: resume-notice.mjs vmap|release|nexus-text');
    return 2;
  }
  const text = notice(argv[0], env);
  if (text) console.log(text);
  return text ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exitCode = main(process.argv.slice(2));
}
