import test from 'node:test';
import assert from 'node:assert/strict';
import { parsePorcelain, evaluateProvenance, UNKNOWN_COMMIT } from './provenance.mjs';

const COMMIT_A = 'a'.repeat(40);
const COMMIT_B = 'b'.repeat(40);

test('clean worktree produces an official, successful provenance', () => {
  const status = parsePorcelain('');
  assert.deepEqual(status, { clean: true, changes: [] });

  const result = evaluateProvenance({
    gitAvailable: true,
    gitCommit: COMMIT_A,
    worktreeClean: status.clean,
    embeddedCommit: COMMIT_A,
  });
  assert.equal(result.ok, true);
  assert.equal(result.official, true);
  assert.equal(result.reason, 'clean');
});

test('dirty tracked file fails the provenance check', () => {
  const status = parsePorcelain(' M components/vocal_fx/src/vocal_fx.cpp\n');
  assert.equal(status.clean, false);
  assert.deepEqual(status.changes, [' M components/vocal_fx/src/vocal_fx.cpp']);

  const result = evaluateProvenance({
    gitAvailable: true,
    gitCommit: COMMIT_A,
    worktreeClean: status.clean,
    embeddedCommit: COMMIT_A,
  });
  assert.equal(result.ok, false);
  assert.equal(result.error, 'dirty-worktree');
  assert.match(result.message, /dirty worktree/i);
  assert.match(result.message, /Commit or stash/);
});

test('build outputs only are not reported as dirty', () => {
  // `git status --porcelain --untracked-files=no` never lists ignored build
  // outputs, so a tree with only build-wasm/ present yields empty output.
  const status = parsePorcelain('');
  assert.equal(status.clean, true);
  assert.equal(status.changes.length, 0);
});

test('CRLF porcelain output is handled', () => {
  const status = parsePorcelain(' M a.cpp\r\n?? b.txt\r\n');
  assert.equal(status.clean, false);
  assert.deepEqual(status.changes, [' M a.cpp', '?? b.txt']);
});

test('embedded commit mismatch fails even on a clean tree', () => {
  const result = evaluateProvenance({
    gitAvailable: true,
    gitCommit: COMMIT_A,
    worktreeClean: true,
    embeddedCommit: COMMIT_B,
  });
  assert.equal(result.ok, false);
  assert.equal(result.error, 'commit-mismatch');
  assert.match(result.message, /does not match|stale|different tree/i);
});

test('official build fails when the embedded commit is null', () => {
  const result = evaluateProvenance({
    gitAvailable: true,
    gitCommit: COMMIT_A,
    worktreeClean: true,
    embeddedCommit: null,
  });
  assert.equal(result.ok, false);
  assert.equal(result.official, true);
  assert.equal(result.error, 'invalid-embedded-commit');
  assert.match(result.message, /missing or malformed/i);
});

test('official build fails for empty, unknown or malformed embedded commits', () => {
  const badValues = ['', UNKNOWN_COMMIT, 'abc123', 'A'.repeat(40), 'g'.repeat(40), 'a'.repeat(39)];
  for (const bad of badValues) {
    const result = evaluateProvenance({
      gitAvailable: true,
      gitCommit: COMMIT_A,
      worktreeClean: true,
      embeddedCommit: bad,
    });
    assert.equal(result.ok, false, `expected failure for ${JSON.stringify(bad)}`);
    assert.equal(result.error, 'invalid-embedded-commit', `wrong error for ${JSON.stringify(bad)}`);
  }
});

test('git unavailable yields a documented, non-official fallback', () => {
  const result = evaluateProvenance({
    gitAvailable: false,
    gitCommit: UNKNOWN_COMMIT,
    worktreeClean: true,
    embeddedCommit: 'whatever',
  });
  assert.equal(result.ok, true);
  assert.equal(result.official, false);
  assert.equal(result.reason, 'git-unavailable');
});
