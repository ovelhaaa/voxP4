/**
 * Pure, dependency-free provenance helpers for the VoxP4 WASM package.
 *
 * These functions describe the rules used to decide whether a WASM package may
 * be published as an official, reproducible artifact. They are intentionally
 * isolated from git/fs so they can be unit-tested deterministically.
 */

export const UNKNOWN_COMMIT = 'unknown';

/** A canonical git commit: 40 lowercase hex characters. */
export const COMMIT_PATTERN = /^[0-9a-f]{40}$/;

export function isValidCommit(value) {
  return typeof value === 'string' && COMMIT_PATTERN.test(value);
}

/**
 * Parses the output of:
 *   git status --porcelain --untracked-files=no
 *
 * Because `--untracked-files=no` is used, build outputs and other ignored or
 * untracked files never appear here: only modifications/additions/removals of
 * tracked files count as a dirty worktree.
 *
 * @param {string} porcelainOutput
 * @returns {{ clean: boolean, changes: string[] }}
 */
export function parsePorcelain(porcelainOutput) {
  const changes = String(porcelainOutput ?? '')
    .split('\n')
    .map((line) => line.replace(/\r$/, ''))
    .filter((line) => line.length > 0);
  return { clean: changes.length === 0, changes };
}

/**
 * Decides whether provenance is acceptable for an official build.
 *
 * Official (git-backed) builds require ALL of:
 *   1. git HEAD is known;
 *   2. the tracked worktree is clean;
 *   3. the commit embedded in the binary is a valid 40-char lowercase hex SHA
 *      (absent / empty / "unknown" / malformed is rejected);
 *   4. the embedded commit equals git HEAD.
 *
 * The only exemption is a truly git-less environment: the build may still
 * proceed with `dspCommit = unknown`, but it is explicitly marked as
 * non-official (not a reproducible editor artifact).
 *
 * @param {{ gitAvailable: boolean, gitCommit: string, worktreeClean: boolean, embeddedCommit: string | null | undefined }} input
 * @returns {{ ok: true, official: boolean, reason: string } | { ok: false, official: boolean, error: string, message: string }}
 */
export function evaluateProvenance({ gitAvailable, gitCommit, worktreeClean, embeddedCommit }) {
  if (!gitAvailable) {
    return { ok: true, official: false, reason: 'git-unavailable' };
  }

  if (!worktreeClean) {
    return {
      ok: false,
      official: true,
      error: 'dirty-worktree',
      message:
        'Cannot generate a reproducible VoxP4 WASM package from a dirty worktree.\n' +
        'Commit or stash source changes before producing editor artifacts.',
    };
  }

  if (!isValidCommit(embeddedCommit)) {
    return {
      ok: false,
      official: true,
      error: 'invalid-embedded-commit',
      message:
        `WASM embedded dspCommit is missing or malformed (got ${JSON.stringify(embeddedCommit)}).\n` +
        'An official build must embed a 40-char lowercase hex git commit.',
    };
  }

  if (embeddedCommit !== gitCommit) {
    return {
      ok: false,
      official: true,
      error: 'commit-mismatch',
      message:
        `WASM embeds dspCommit ${embeddedCommit} but git HEAD is ${gitCommit}.\n` +
        'The WASM build is stale or was produced from a different tree. Rebuild before generating the manifest.',
    };
  }

  return { ok: true, official: true, reason: 'clean' };
}
