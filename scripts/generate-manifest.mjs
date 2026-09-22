import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const wasmDir = path.join(__dirname, '..', 'build-wasm');
const wasmFile = path.join(wasmDir, 'voxp4-preview.wasm');
const mjsFile = path.join(wasmDir, 'voxp4-preview.mjs');

if (!fs.existsSync(wasmFile) || !fs.existsSync(mjsFile)) {
  console.error('WASM artifacts not found in', wasmDir);
  process.exit(1);
}

const createModule = (await import('file:///' + mjsFile.replace(/\\/g, '/'))).default;
const mod = await createModule({ wasmBinary: fs.readFileSync(wasmFile) });
const manifestStr = mod.cwrap('voxp4_preview_get_manifest_json', 'string', [])();

const targetDirs = [
  path.join(__dirname, '..', '..', 'voxP4-editor', 'src', 'audio', 'wasm'),
  path.join(__dirname, '..', '..', 'voxP4-editor', 'public', 'wasm'),
];

for (const dir of targetDirs) {
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'dsp-compatibility.json'), manifestStr, 'utf-8');
  console.log(`Saved manifest to ${path.join(dir, 'dsp-compatibility.json')}`);
}
