#!/usr/bin/env node
/**
 * Generates the atomic WASM preview package manifest
 * (build-wasm/dsp-compatibility.json) from the actual build outputs.
 *
 * Sources of truth:
 *   - dspCommit     : git rev-parse HEAD (the same value compiled into the WASM)
 *   - wasmSha256    : SHA-256 of build-wasm/voxp4-preview.wasm
 *   - contractSha256: SHA-256 of the canonical V1 parameter contract
 *   - contractVersion / parameterCount : parsed from the contract
 *   - sampleRate / blockSize / profile / engine : fixed build configuration
 *
 * The contract path can be overridden with VOXP4_CONTRACT_PATH; otherwise the
 * sibling voxP4-editor checkout is used when available. When the contract cannot
 * be located the manifest is still emitted (with contract fields null) so CI
 * always publishes a complete three-file artifact; `npm run verify:wasm` in the
 * editor rejects any package missing real contract provenance.
 */
import fs from 'fs';
import path from 'path';
import crypto from 'crypto';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.join(__dirname, '..');
const wasmDir = path.join(repoRoot, 'build-wasm');
const wasmFile = path.join(wasmDir, 'voxp4-preview.wasm');
const mjsFile = path.join(wasmDir, 'voxp4-preview.mjs');
const manifestFile = path.join(wasmDir, 'dsp-compatibility.json');

if (!fs.existsSync(wasmFile) || !fs.existsSync(mjsFile)) {
  console.error('[generate-manifest] WASM artifacts not found in', wasmDir);
  process.exit(1);
}

const wasmBytes = fs.readFileSync(wasmFile);
const wasmSha256 = crypto.createHash('sha256').update(wasmBytes).digest('hex');

function resolveGitCommit() {
  try {
    return execFileSync('git', ['rev-parse', 'HEAD'], {
      cwd: repoRoot,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
  } catch {
    return 'unknown';
  }
}

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

const gitCommit = resolveGitCommit();

// The commit embedded in the binary MUST match the repository HEAD used to
// produce the manifest. A mismatch means the manifest would declare a
// different provenance than the real binary, which is fatal.
//
// Exception: when Git is unavailable the compiler embeds "unknown" and there is
// no authoritative HEAD to compare against; that case is explicitly allowed and
// documented here.
try {
  const createModule = (await import('file:///' + mjsFile.replace(/\\/g, '/'))).default;
  const mod = await createModule({ wasmBinary: wasmBytes });
  const embedded = JSON.parse(mod.cwrap('voxp4_preview_get_manifest_json', 'string', [])());

  if (gitCommit !== 'unknown' && embedded.dspCommit && embedded.dspCommit !== gitCommit) {
    console.error(
      `[generate-manifest] ERROR: embedded dspCommit ${embedded.dspCommit} does not match git HEAD ${gitCommit}.\n` +
        '  The WASM build is stale or was produced from a different tree. Rebuild before generating the manifest.'
    );
    process.exit(1);
  }
} catch (err) {
  console.error(
    `[generate-manifest] ERROR: could not inspect the embedded manifest to verify provenance: ${err.message}`
  );
  process.exit(1);
}

const contractFile = resolveContractFile();
let contractVersion = null;
let contractSha256 = null;
let parameterCount = null;

if (contractFile) {
  const contractBytes = fs.readFileSync(contractFile);
  contractSha256 = crypto.createHash('sha256').update(contractBytes).digest('hex');
  const contract = JSON.parse(contractBytes.toString('utf8'));
  contractVersion = contract.contractVersion ?? null;
  parameterCount = Array.isArray(contract.parameters)
    ? contract.parameters.length
    : (contract.parameterCount ?? null);
  console.log(`[generate-manifest] Contract: ${contractFile}`);
} else {
  console.warn(
    '[generate-manifest] WARNING: canonical parameter contract not found. ' +
      'Set VOXP4_CONTRACT_PATH to include contract provenance in the manifest.'
  );
}

const manifest = {
  engine: 'voxP4',
  dspCommit: gitCommit,
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
if (manifest.contractSha256) {
  console.log(`  contract   = ${manifest.contractSha256}`);
}
