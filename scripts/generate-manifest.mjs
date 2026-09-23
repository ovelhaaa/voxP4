#!/usr/bin/env node
/**
 * Generates the WASM preview package manifest
 * (build-wasm/dsp-compatibility.json) from the actual build outputs.
 *
 * An official, reproducible package requires ALL of:
 *   - git HEAD is known;
 *   - the tracked worktree is clean (ignored build outputs do not count);
 *   - the commit embedded in the WASM equals git HEAD;
 *   - the canonical parameter contract is available.
 *
 * If any of these fail, no manifest is written and the process exits non-zero.
 * Only when every check passes is the manifest assembled in memory and written.
 *
 * Sources of truth:
 *   - dspCommit      : git rev-parse HEAD (the same value compiled into the WASM)
 *   - wasmSha256     : SHA-256 of build-wasm/voxp4-preview.wasm
 *   - contractSha256 : SHA-256 of the canonical V1 parameter contract
 *   - contractVersion / parameterCount : parsed from the contract
 *   - sampleRate / blockSize / profile / engine : fixed build configuration
 *
 * The contract path can be overridden with VOXP4_CONTRACT_PATH; otherwise the
 * sibling voxP4-editor checkout is used. A git-less build may still emit a
 * manifest with dspCommit "unknown", but that package is NOT an official
 * reproducible artifact and must not be synced to the editor.
 */
import fs from 'fs';
import path from 'path';
import crypto from 'crypto';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';
import { parsePorcelain, evaluateProvenance, UNKNOWN_COMMIT } from './lib/provenance.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.join(__dirname, '..');
const wasmDir = path.join(repoRoot, 'build-wasm');
const wasmFile = path.join(wasmDir, 'voxp4-preview.wasm');
const mjsFile = path.join(wasmDir, 'voxp4-preview.mjs');
const manifestFile = path.join(wasmDir, 'dsp-compatibility.json');

function fail(message) {
  console.error(`[generate-manifest] ERROR: ${message}`);
  process.exit(1);
}

if (!fs.existsSync(wasmFile) || !fs.existsSync(mjsFile)) {
  fail(`WASM artifacts not found in ${wasmDir}`);
}

const wasmBytes = fs.readFileSync(wasmFile);
const wasmSha256 = crypto.createHash('sha256').update(wasmBytes).digest('hex');

// ---------------------------------------------------------------------------
// 1. Git provenance (commit + tracked-worktree cleanliness)
// ---------------------------------------------------------------------------
function resolveGitState() {
  try {
    const gitCommit = execFileSync('git', ['rev-parse', 'HEAD'], {
      cwd: repoRoot,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();

    const porcelain = execFileSync(
      'git',
      ['status', '--porcelain', '--untracked-files=no'],
      {
        cwd: repoRoot,
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore'],
      }
    );
    const { clean, changes } = parsePorcelain(porcelain);
    return { gitAvailable: true, gitCommit, worktreeClean: clean, changes };
  } catch {
    return { gitAvailable: false, gitCommit: UNKNOWN_COMMIT, worktreeClean: true, changes: [] };
  }
}

const gitState = resolveGitState();

// ---------------------------------------------------------------------------
// 2. Embedded commit inspection (must not fail silently)
// ---------------------------------------------------------------------------
let embeddedCommit = null;
try {
  const createModule = (await import('file:///' + mjsFile.replace(/\\/g, '/'))).default;
  const mod = await createModule({ wasmBinary: wasmBytes });
  const embedded = JSON.parse(mod.cwrap('voxp4_preview_get_manifest_json', 'string', [])());
  embeddedCommit = embedded.dspCommit ?? null;
} catch (err) {
  fail(`could not inspect the embedded manifest to verify provenance: ${err.message}`);
}

// ---------------------------------------------------------------------------
// 3. Provenance decision (HEAD known + clean tree + embedded == HEAD)
// ---------------------------------------------------------------------------
const verdict = evaluateProvenance({
  gitAvailable: gitState.gitAvailable,
  gitCommit: gitState.gitCommit,
  worktreeClean: gitState.worktreeClean,
  embeddedCommit,
});

if (!verdict.ok) {
  fail(verdict.message);
}

if (verdict.official) {
  console.log(`[generate-manifest] git HEAD ${gitState.gitCommit} (worktree clean)`);
} else {
  console.warn(
    '[generate-manifest] WARNING: git unavailable; emitting a NON-OFFICIAL manifest (dspCommit=unknown). ' +
      'This package is not a reproducible editor artifact.'
  );
}

// ---------------------------------------------------------------------------
// 4. Contract provenance (required for an official package)
// ---------------------------------------------------------------------------
function resolveContractFile() {
  const candidates = [
    process.env.VOXP4_CONTRACT_PATH,
    path.join(repoRoot, '..', 'voxP4-editor', 'contracts', 'voxp4-parameters-v1.json'),
    path.join(repoRoot, 'voxP4-editor', 'contracts', 'voxp4-parameters-v1.json'),
  ].filter(Boolean);

  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) return candidate;
  }
  return null;
}

const contractFile = resolveContractFile();
if (!contractFile) {
  fail(
    'canonical parameter contract not found. Set VOXP4_CONTRACT_PATH to the editor ' +
      'contracts/voxp4-parameters-v1.json before generating an official package.'
  );
}

const contractBytes = fs.readFileSync(contractFile);
const contractSha256 = crypto.createHash('sha256').update(contractBytes).digest('hex');
const contract = JSON.parse(contractBytes.toString('utf8'));
const contractVersion = contract.contractVersion ?? null;
const parameterCount = Array.isArray(contract.parameters)
  ? contract.parameters.length
  : (contract.parameterCount ?? null);
console.log(`[generate-manifest] Contract: ${contractFile}`);

// ---------------------------------------------------------------------------
// 5. Assemble and write only after every validation has passed
// ---------------------------------------------------------------------------
const manifest = {
  engine: 'voxP4',
  dspCommit: gitState.gitCommit,
  wasmSha256,
  contractVersion,
  contractSha256,
  parameterCount,
  sampleRate: 48000,
  blockSize: 64,
  profile: 'P4Production',
};

fs.writeFileSync(manifestFile, JSON.stringify(manifest, null, 2) + '\n', 'utf-8');
console.log(`[generate-manifest] Wrote ${manifestFile}`);
console.log(`  dspCommit  = ${manifest.dspCommit}`);
console.log(`  wasmSha256 = ${manifest.wasmSha256}`);
console.log(`  contract   = ${manifest.contractSha256}`);
