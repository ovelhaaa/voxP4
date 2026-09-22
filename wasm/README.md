# VoxP4 WebAssembly DSP Preview Engine

Este módulo compila o DSP real do VoxP4 (`vocal_fx`) para WebAssembly utilizando Emscripten, permitindo que o `voxP4-editor` processe e reproduza prévias de áudio de presets e scenes com fidelidade idêntica ao hardware ESP32-P4, diretamente no navegador.

---

## 1. Arquitetura

* **Perfil de DSP**: `VocalFxPlatformProfile::P4Production` (mesmo profile do produto final ESP32-P4, incluindo YIN incremental, pitch marks NCC `ContiguousMulti8` e kernels otimizados de PSOLA/LPC).
* **Parâmetros**: 71 parâmetros canônicos indexados por chaves semânticas (`key`), ligando diretamente o contrato V1 (`contracts/voxp4-parameters-v1.json`) ao registro canônico (`voxlink_registry.cpp`) e ao engine (`vocal_fx_param_binding.cpp`).
* **Processamento**: Executado em blocos de 64 amostras a 48 kHz. O loop completo de renderização e análise de pitch é executado inteiramente em C++ no interior do WebAssembly, eliminando overhead de chamadas JS-WASM.
* **Tamanho**: Binário WASM de aproximadamente 190 KB.

---

## 2. API C / WASM Exportada

```c
// Inicializa o engine com perfil P4Production
bool voxp4_preview_init(float sample_rate, uint32_t block_size);

// Define valor de parâmetro pela chave semântica (ex: "delay.wet", "reverb.decay_s")
bool voxp4_preview_set_parameter(const char *semantic_key, float value);

// Reseta todos os parâmetros para os defaults de fábrica
bool voxp4_preview_reset_parameters();

// Reseta buffers internos de delay, reverb e histórico de pitch
void voxp4_preview_reset();

// Renderiza buffer mono para estéreo (L e R)
bool voxp4_preview_render(const float *input, size_t frames, float *output_l, float *output_r);

// Versão do engine de preview
const char *voxp4_preview_version();

// Manifesto JSON de compatibilidade DSP
const char *voxp4_preview_get_manifest_json();
```

---

## 3. Como Compilar

Requisitos:
* Emscripten SDK (`emsdk`)
* CMake >= 3.16
* Ninja

### No Windows:
```cmd
C:\emsdk\emsdk_env.bat
emcmake cmake -B build-wasm -S wasm -G Ninja
cmake --build build-wasm
```

### No Linux / macOS:
```bash
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-wasm -S wasm -G Ninja
cmake --build build-wasm
```

Os artefatos gerados são:
* `build-wasm/voxp4-preview.mjs` (Wrapper ES Module)
* `build-wasm/voxp4-preview.wasm` (Binário WebAssembly)
* `build-wasm/dsp-compatibility.json` (Manifesto de compatibilidade)

No Windows, `scripts/build-wasm.bat` executa o build, gera o manifesto e
sincroniza os três arquivos atomicamente em `voxP4-editor/src/audio/wasm`
através de `voxP4-editor/scripts/sync-wasm.mjs`.

---

## 4. Manifesto de Compatibilidade

O commit DSP real é injetado pelo CMake em tempo de configuração
(`git rev-parse HEAD`, com fallback para `unknown`) e embutido no binário via
`voxp4_preview_get_manifest_json()`. O script `scripts/generate-manifest.mjs`
produz `build-wasm/dsp-compatibility.json` combinando:

* `dspCommit` (commit Git do build);
* `wasmSha256` (SHA-256 do `.wasm`);
* `contractSha256`, `contractVersion`, `parameterCount` (do contrato canônico
  `contracts/voxp4-parameters-v1.json` do editor);
* `engine`, `sampleRate`, `blockSize`, `profile` (configuração fixa do build).

O editor valida esse manifesto com `npm run verify:wasm`, que recalcula os
hashes reais e falha em qualquer divergência.
