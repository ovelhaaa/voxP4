# ESP32-P4 — Vocal FX Engine

**Especificação técnica para multi-efeitos vocal em tempo real**  
Harmonizer • Pitch Correction • Doubler • Delay • FDN Reverb • Vocoder  
**Plataforma alvo:** ESP32-P4 @ 400 MHz • 48 kHz • I2S/DMA • ESP-IDF  
**Revisão 1.0 — Setembro de 2026**

## 1. Visão geral

Este documento especifica um engine de processamento vocal em tempo real para ESP32-P4, orientado a uso musical ao vivo. O objetivo é combinar uma cadeia vocal convencional com harmonização de 1–2 vozes, correção de afinação, doubler, delay estéreo, reverb FDN de alta qualidade e integração MIDI, preservando baixa latência e margem de CPU para expansão.

> **Decisão principal:** a arquitetura recomendada usa YIN/MPM para análise de F0, TD-PSOLA como pitch shifter principal em regiões voiced, WSOLA/pass-through para regiões unvoiced/transientes e um FDN modulado de 8 linhas para reverb. Phase vocoder fica reservado para um modo HQ opcional.

### 1.1 Objetivos

- Operar a 48 kHz com entrada mono e saída estéreo.
- Manter o caminho dry com latência idealmente abaixo de 5 ms.
- Gerar 1–2 vozes de harmonia em tempo real com latência musicalmente aceitável (~15–30 ms).
- Fornecer harmonização fixed interval, diatônica e guiada por MIDI.
- Oferecer pitch correction com retune speed configurável.
- Fornecer reverb FDN de qualidade claramente superior a arquiteturas Schroeder/Freeverb básicas.
- Permitir integração posterior com AMY, inclusive como carrier para vocoder.
- Manter módulos desacoplados para profiling, testes A/B e substituição de algoritmos.

### 1.2 Fora de escopo da primeira versão

- Separação de fontes por redes neurais.
- Harmonização polifônica baseada em reconhecimento automático de acordes complexos.
- Preservação de formantes de nível estúdio na primeira milestone.
- Convolution reverb longa como reverb principal.
- Port integral de bibliotecas desktop como Rubber Band.

## 2. Plataforma e orçamento de sistema

A implementação é direcionada ao ESP32-P4 dual-core RISC-V de alto desempenho, usando I2S com DMA, memória interna para buffers críticos e PSRAM apenas para buffers longos ou dados não críticos. A estratégia inicial deve ser `float32` por clareza e velocidade de desenvolvimento; fixed-point ou intrinsics Xai só entram após profiling.

| Recurso | Alvo de projeto | Uso proposto |
|---|---|---|
| CPU | 2 × core RISC-V até 400 MHz | Core 0: áudio RT; Core 1: análise/controle |
| Sample rate | 48.000 Hz | Padrão único da pipeline |
| Audio block | 64 ou 128 samples | 1,33 ms ou 2,67 ms por callback |
| Formato interno | `float32` | Primeira implementação |
| Entrada | Mono | Microfone/line via codec I2S |
| Saída | Stereo | Dry + harmonias + time FX |
| RAM interna | Prioridade para hot path | DMA, PSOLA, estados, FDN se couber |
| PSRAM | Opcional | Long delays, assets, UI, presets |

## 3. Arquitetura de alto nível

```text
MIC / CODEC
    |
    v
[ I2S DMA ] --> [ Input DSP ] -->+-----------------------> [ Dry ] ----+
                                 |                                    |
                                 +--> [ Analysis tap ]                 |
                                           |                           |
                                           v                           |
                                    [ YIN / MPM ]                      |
                                           | F0/confidence             |
                                           v                           |
MIDI / scale / key ----------> [ Harmony Engine ]                     |
                                           | targets                   |
                                   +-------+-------+                   |
                                   v               v                   |
                              [TD-PSOLA 1]   [TD-PSOLA 2]              |
                                   |               |                   |
                                   +-------+-------+                   |
                                           v                           |
                                       [ Mixer ] <---------------------+
                                           |
                                    [ Doubler/Chorus ]
                                           |
                                     [ Stereo Delay ]
                                           |
                                      [ 8-line FDN ]
                                           |
                                        [Limiter]
                                           |
                                       [I2S Stereo]
```

### 3.1 Particionamento entre cores

| Core 0 — hard real-time | Core 1 — análise/controle |
|---|---|
| I2S DMA / callback | YIN/MPM |
| Input DSP | Voiced/unvoiced decision |
| TD-PSOLA synthesis | Harmony target calculation |
| Mixer / doubler / delay | MIDI parsing |
| FDN reverb / limiter | UI / presets / telemetry |
| Sem alocação dinâmica no callback | Pode trabalhar em janelas rolling |

> **Regra de RT:** nenhuma alocação, `printf`, filesystem, mutex bloqueante ou operação potencialmente não determinística dentro do callback de áudio. Comunicação entre cores via ring buffer lock-free e snapshots atômicos de parâmetros.

## 4. Modelo de dados e APIs

### 4.1 API pública do engine

```c
typedef struct {
    uint32_t sample_rate;      // 48000
    uint16_t block_size;       // 64 or 128
    uint8_t harmony_voices;    // 0..2 initially
    bool enable_reverb;
    bool enable_delay;
    bool enable_pitch_correct;
} vocal_fx_config_t;

typedef struct {
    float detected_hz;
    float confidence;          // 0..1
    bool voiced;
    uint32_t age_samples;
} pitch_state_t;

esp_err_t vocal_fx_init(const vocal_fx_config_t *cfg);
void vocal_fx_reset(void);
void vocal_fx_process(const float *in, float *out_l, float *out_r, size_t frames);
void vocal_fx_set_param(uint16_t id, float value);
void vocal_fx_midi_note_on(uint8_t note, uint8_t velocity);
void vocal_fx_midi_note_off(uint8_t note);
```

### 4.2 Interfaces internas

```c
PitchResult pitch_detector_process(const float *analysis_frame, size_t n);
void harmony_update_targets(const PitchResult *pitch, HarmonyTargets *out);
void td_psola_set_target(TdPsola *s, float ratio);
void td_psola_process(TdPsola *s, const float *in, float *out, size_t n);
void fdn_process(FdnReverb *r, const float *in, float *out_l, float *out_r, size_t n);
```

## 5. Análise de pitch (F0)

O analisador deve rodar fora do hard real-time. Recomenda-se derivar um tap do áudio após HPF leve, aplicar low-pass e decimar para 12 kHz ou 24 kHz, preservando o callback principal em 48 kHz.

### 5.1 YIN — implementação de referência

```c
for (tau = tau_min; tau <= tau_max; ++tau) {
    float acc = 0.0f;
    for (i = 0; i < N - tau; ++i) {
        float d = x[i] - x[i + tau];
        acc += d * d;
    }
    diff[tau] = acc;
}

cmnd[0] = 1.0f;
float running = 0.0f;
for (tau = 1; tau <= tau_max; ++tau) {
    running += diff[tau];
    cmnd[tau] = (running > 0.0f) ? diff[tau] * tau / running : 1.0f;
}

// first local minimum below threshold, then parabolic interpolation
```

| Parâmetro | Valor inicial | Observação |
|---|---|---|
| Analysis rate | 12 kHz | Decimation ×4 a partir de 48 kHz |
| F0 min | 70 Hz | Voz grave; `tau_max` ~171 @ 12 kHz |
| F0 max | 1000 Hz | Cobertura ampla de voz/falsete |
| Window | 25–40 ms | Rolling; não bloqueia a pipeline |
| Update | ~5 ms | Novo F0/confidence |
| YIN threshold | 0,10–0,20 | Calibrar com corpus vocal |
| Confidence hold | 20–50 ms | Evita dropouts instantâneos |

### 5.2 MPM como segundo backend

O McLeod Pitch Method deve ser mantido como backend alternativo atrás da mesma interface. O objetivo não é decidir por teoria, mas por benchmark no P4 com gravações reais: erro de oitava, estabilidade em vibrato, ruído de palco e custo de CPU.

### 5.3 Voiced/unvoiced

A decisão de periodicidade é essencial para evitar que PSOLA destrua consoantes e transientes. A classificação deve combinar confidence do detector, energia, periodicidade e opcionalmente spectral flatness simplificada.

```c
voiced = (pitch.confidence > conf_min) &&
         (rms > rms_floor) &&
         (periodicity > periodicity_min);

if (!voiced) {
    // preserve consonants: dry / WSOLA-light path
}
```

## 6. Pitch shifting ao vivo — TD-PSOLA

TD-PSOLA é o engine principal da primeira versão por ser especialmente adequado a fala/voz periódica, exigir pouca memória e permitir latência menor que um phase vocoder de janela longa.

### 6.1 Pitch marks

Nunca usar apenas marcas equidistantes derivadas de F0. A posição prevista deve ser refinada por busca local para alinhar cada marca a uma fase coerente da forma de onda, preferencialmente por correlação com o período anterior.

```c
predicted = previous_mark + estimated_period;
best = predicted;
best_score = -INF;

for (offset = -search; offset <= search; ++offset) {
    candidate = predicted + offset;
    score = normalized_corr(previous_mark, candidate, period);
    if (score > best_score) {
        best_score = score;
        best = candidate;
    }
}
```

### 6.2 Overlap-add

```c
float ratio = powf(2.0f, semitones / 12.0f);
float target_period = detected_period / ratio;

while (synth_pos < block_end) {
    int mark = nearest_source_mark(analysis_pos);
    int P = period_at(mark);
    int W = 2 * P;

    for (int n = -P; n < P; ++n) {
        float w = hann(n + P, W);
        out[synth_pos + n] += in[mark + n] * w;
        norm[synth_pos + n] += w;
    }

    synth_pos += target_period;
    analysis_pos += detected_period;
}
```

| Intervalo | Ratio | Uso típico |
|---|---:|---|
| +3 st | 1.1892 | terça menor fixa |
| +4 st | 1.2599 | terça maior fixa |
| +7 st | 1.4983 | quinta |
| -3 st | 0.8409 | harmonia inferior |
| -5 st | 0.7492 | quarta inferior |
| +12 st | 2.0000 | efeito/octave; maior risco de formantes |

## 7. WSOLA e tratamento de transientes

WSOLA deve complementar PSOLA, não substituí-lo. Em regiões unvoiced e em consoantes, a prioridade é preservar inteligibilidade e transientes. A correlação WSOLA é um excelente candidato para otimização vetorial no P4.

```c
for (offset = -search; offset <= search; ++offset) {
    float corr = dot(previous_overlap,
                     input + candidate + offset,
                     overlap_len);
    if (corr > best_corr) {
        best_corr = corr;
        best_offset = offset;
    }
}

crossfade(previous_overlap,
          input + candidate + best_offset,
          output);
```

> **Estratégia híbrida:** voiced estável → TD-PSOLA. Unvoiced/transiente → dry ou WSOLA-light. Na fronteira, crossfade suave entre caminhos durante alguns milissegundos.

## 8. Harmony Engine

### 8.1 Modos

| Modo | Entrada musical | Cálculo do target |
|---|---|---|
| Fixed interval | Sem tonalidade | F0 × 2^(semitones/12) |
| Diatonic | Key + scale | Mapear degree atual para degree de harmonia |
| MIDI chord | Notas MIDI held | Selecionar chord tones próximos da voz |
| Pitch correction | Key + scale | Quantizar F0 para nota permitida |
| Hard tune | Key + scale | Quantização rápida + glide mínimo |

### 8.2 Smoothing em cents

```c
float current_cents = hz_to_cents(current_hz);
float target_cents  = hz_to_cents(target_hz);
current_cents += alpha * (target_cents - current_cents);
ratio = cents_to_ratio(current_cents - detected_cents);
```

| Preset | Tempo de glide sugerido |
|---|---|
| Natural harmony | 50–100 ms |
| Tight harmony | 20–50 ms |
| Pitch correction natural | 30–80 ms |
| Hard tune | 5–20 ms |

## 9. Preservação de formantes — fase 2

Na primeira milestone, aceitar a alteração moderada de formantes para intervalos usuais de ±3 a ±7 semitons. Para modo avançado, adicionar análise LPC: separar residual/excitação do envelope do trato vocal, aplicar pitch shift ao residual e ressintetizar pelo filtro LPC.

```text
x[n] --> inverse LPC filter --> residual e[n]
                               |
                               v
                           pitch shift
                               |
                               v
                        LPC synthesis filter
                               |
                               v
                              y[n]
```

| Item | Valor inicial |
|---|---|
| Ordem LPC @48 kHz | 16–24 |
| Update de coeficientes | 5–10 ms |
| Interpolação de coeficientes | Obrigatória entre frames |
| Uso inicial | Somente harmony path, não dry |

## 10. Input DSP

| Bloco | Implementação sugerida | Parâmetros iniciais |
|---|---|---|
| HPF | Biquad 2ª ordem | 70–100 Hz |
| Gate/expander | Envelope peak/RMS | release 80–200 ms |
| Compressor | Soft knee | attack 5–15 ms; release 60–150 ms |
| EQ | 3–4 biquads | low shelf / 2 peaks / high shelf |
| De-esser | Bandpass side-chain | ~4–10 kHz, compressão seletiva |

## 11. Doubler, micro-pitch e delay

Doubler deve ser implementado como um efeito barato e musicalmente útil mesmo quando o harmonizer está desligado. Use delays curtos assimétricos e detune de poucos cents; para ±cents pode ser usado um granular simples sem a complexidade total do PSOLA.

| Tap | Delay | Pitch | Pan |
|---|---|---|---|
| Dry | 0 ms | 0 cents | center |
| Double L | ~15–20 ms | -4 a -8 cents | left |
| Double R | ~22–30 ms | +4 a +8 cents | right |

### 11.1 Stereo delay

- Tempo livre em ms e sincronizado ao BPM.
- Feedback com low-pass/high-pass no loop.
- Ping-pong opcional.
- Duck do delay controlado pela energia da voz dry.
- Buffers longos podem residir em PSRAM se medição mostrar acesso estável.

## 12. Reverb FDN modulado de 8 linhas

O reverb principal deve ser um Feedback Delay Network com 8 linhas, matriz de mistura Hadamard/ortogonal, damping por linha, diffusion de entrada, modulação lenta e taps de saída estéreo decorrelacionados.

```text
input -> diffuser -> [D1]--LP--+
                    [D2]--LP--+
                    [D3]--LP--+
                    [D4]--LP--+--> Hadamard 8x8 --> feedback gains --> delays
                    [D5]--LP--+
                    [D6]--LP--+
                    [D7]--LP--+
                    [D8]--LP--+
                                  |
                                  +--> stereo output matrix
```

### 12.1 Configuração inicial

| Parâmetro | Valor inicial |
|---|---|
| Delay lines | 8 |
| Comprimentos base | ~31–51 ms, mutuamente pouco correlacionados |
| Feedback matrix | Hadamard normalizada |
| Damping | 1-pole LP por linha |
| Modulation | ±1–3 samples, LFOs lentos e decorrelacionados |
| Predelay | 0–120 ms |
| Input diffusion | 2–4 allpass curtos |
| Output | Matriz stereo decorrelacionada |
| RT60 | Derivado de gains por banda / preset |

### 12.2 Hadamard eficiente

```c
// butterfly stages; only add/subtract
h0 = x0 + x1; h1 = x0 - x1;
h2 = x2 + x3; h3 = x2 - x3;
h4 = x4 + x5; h5 = x4 - x5;
h6 = x6 + x7; h7 = x6 - x7;
// additional butterfly stages...
// normalize by 1/sqrt(8)
```

### 12.3 Presets alvo

| Preset | Características |
|---|---|
| Vocal Plate | Denso, brilho moderado, predelay 20–60 ms |
| Small Room | Curto, early reflections evidentes |
| Large Hall | RT60 longo, damping progressivo |
| Dark Hall | HF decay curto, cauda longa |
| Ambient | Mais diffusion e modulação |
| Shimmer | Pitch +12 no feedback em bloco opcional |

## 13. Phase Vocoder HQ — módulo opcional

O phase vocoder não deve bloquear o MVP. Ele serve como modo de maior qualidade/maior latência, útil para intervalos grandes e experimentação com phase locking e tratamento de transientes.

| Configuração | FFT | Hop | Janela nominal | Comentário |
|---|---:|---:|---:|---|
| Live experimental | 512 | 128 | 10,7 ms | Menor latência, pior resolução espectral |
| HQ | 1024 | 256 | 21,3 ms | Melhor equilíbrio qualidade/custo |
| HQ+ | 2048 | 512 | 42,7 ms | Provável excesso para monitoramento ao vivo |

> **Critério de entrada:** só desenvolver o modo HQ após a versão TD-PSOLA atingir estabilidade de F0, consoantes aceitáveis e duas vozes com margem de CPU.

## 14. Orçamento de memória

| Bloco | Estimativa | Memória recomendada |
|---|---|---|
| I2S DMA + interleaving | ~4–16 KB | Interna |
| Audio scratch / mix | ~8–24 KB | Interna |
| YIN/MPM analysis | ~8–32 KB | Interna |
| PSOLA rings + OLA ×2 | ~40–100 KB | Interna |
| FDN 8 linhas | ~80–180 KB | Interna se possível |
| Stereo delay longo | ~0,2–2 MB | PSRAM aceitável |
| Presets/UI/assets | Variável | PSRAM/flash |

**Regra prática:** reservar memória interna para tudo que é acessado a cada sample ou em loops de alto custo; mover para PSRAM somente buffers grandes cujo acesso seja sequencial e cujo comportamento tenha sido medido sob carga.

## 15. Orçamento de CPU e metas de profiling

Os números abaixo são metas/estimativas de engenharia para orientar profiling, não garantias. O objetivo é manter folga suficiente para piores casos e jitter do sistema.

| Bloco | Meta aproximada | Observação |
|---|---|---|
| Input DSP | 1–3% de 1 core | Biquads/dynamics |
| YIN/MPM | 3–10% de 1 core | Core 1, decimado |
| TD-PSOLA 1 voz | 3–10% de 1 core | Depende de correlação e janela |
| 2 vozes PSOLA | 6–20% de 1 core | Hot path |
| Doubler + delay | 1–4% | Baixo custo |
| FDN 8 linhas | 3–8% | Float32; modulação incluída |
| Mixer + limiter | <2% | Sem oversampling inicialmente |
| UI/MIDI/control | <10% Core 1 | Sem bloquear áudio |

> **Budget de segurança:** mirar ≤ 60–65% de carga média no Core 0 e ≤ 60% no Core 1 em stress test, deixando margem para picos, caches, drivers e futura expansão.

## 16. Tarefas, filas e sincronização

| Componente | Prioridade | Mecanismo |
|---|---|---|
| Audio callback/task | Máxima | DMA + processamento in-place/contíguo |
| Analysis task | Alta | Ring buffer SPSC de amostras decimadas |
| MIDI task | Média/alta | Queue curta; atualiza chord state |
| UI/control | Média | Parameter snapshot |
| Storage/presets | Baixa | Nunca no caminho RT |

```c
typedef struct {
    float f0_hz;
    float confidence;
    uint32_t generation;
    bool voiced;
} PitchSnapshot;

// Core 1 publishes atomically; Core 0 reads last complete snapshot.
```

## 17. Sistema de parâmetros e presets

Todos os parâmetros contínuos audíveis devem possuir smoothing interno. A API de controle nunca deve escrever diretamente em estados DSP que causem descontinuidade.

| Grupo | Parâmetros essenciais |
|---|---|
| Input | HPF, gate threshold, comp threshold/ratio/attack/release, de-esser |
| Pitch | mode, key, scale, retune speed, confidence threshold |
| Harmony V1/V2 | interval/degree, level, pan, humanize, delay, formant mode |
| Doubler | amount, delay L/R, cents L/R |
| Delay | time/BPM division, feedback, filter, duck, mix |
| Reverb | preset, predelay, size, decay, damping, modulation, mix |
| Master | dry/wet, output gain, limiter ceiling |

## 18. Plano de testes e benchmarks

### 18.1 Métricas obrigatórias

- CPU média, p95 e pior callback por bloco.
- Número de underruns/overruns em 10, 30 e 60 minutos.
- Latência round-trip medida, não apenas calculada.
- Erro de F0 em cents e taxa de octave error.
- Jitter de pitch marks.
- Tempo para estabilizar após mudança de nota.
- Qualidade subjetiva em vogais, sibilantes, plosivas e vibrato.
- Comportamento com ruído de fundo e backing track vazando no microfone.
- Consumo de memória interna e PSRAM.
- Temperatura/estabilidade sob carga contínua.

### 18.2 Corpus mínimo

| Categoria | Casos |
|---|---|
| Voz masculina | 70–250 Hz; legato; vibrato; ataque |
| Voz feminina | 150–500+ Hz; falsete; sibilância |
| Consoantes | s, f, sh, t, k, p |
| Dinâmica | sussurro a forte |
| Ambiente | ruído branco/rosa; sala; vazamento de monitor |
| Musical | escalas, saltos de terça/quinta/oitava, bends |

## 19. Critérios de aceitação por milestone

| Milestone | Critério de saída |
|---|---|
| M0 — Audio core | 48 kHz estável, 64/128 frames, sem underrun em 60 min |
| M1 — Vocal strip | HPF/EQ/comp/de-esser + dry <5 ms |
| M2 — FDN | Plate/Hall musical, stereo, sem instabilidade em decay máximo |
| M3 — F0 | Tracking utilizável 70–800 Hz, confidence confiável e poucos octave errors |
| M4 — PSOLA 1 voz | ±7 st com vogais estáveis e transientes aceitáveis |
| M5 — 2 vozes | Duas harmonias simultâneas com margem de CPU |
| M6 — Harmony/MIDI | Fixed, diatonic e MIDI chord modes |
| M7 — Pitch correction | Natural + hard tune |
| M8 — Advanced | LPC/formants, shimmer, vocoder ou PV-HQ |

## 20. Roadmap de implementação

1. Criar audio engine I2S/DMA 48 kHz e infraestrutura de profiling.
2. Implementar HPF, compressor, EQ, de-esser e limiter.
3. Implementar stereo delay e FDN 8-line.
4. Adicionar analysis tap decimado e YIN; depois MPM para A/B.
5. Implementar voiced/unvoiced e geração/refino de pitch marks.
6. Implementar TD-PSOLA mono ±7 semitons; validar corpus.
7. Adicionar caminho WSOLA/dry para unvoiced e transientes.
8. Adicionar segunda voz e humanização (delay/cents/level/pan).
9. Implementar Harmony Engine fixed + diatonic + MIDI.
10. Implementar pitch correction / hard tune.
11. Fazer otimizações P4/Xai apenas após profiling.
12. Adicionar LPC/formant preservation como módulo opcional.
13. Adicionar shimmer, vocoder AMY e phase vocoder HQ em milestones posteriores.

## 21. Estrutura de código sugerida

```text
components/vocal_fx/
  include/
    vocal_fx.h
    vocal_fx_params.h
  src/
    vocal_fx.cpp
    audio_graph.cpp
    pitch/
      pitch_detector.h
      yin.cpp
      mpm.cpp
      voiced_detector.cpp
    shift/
      pitch_shifter.h
      td_psola.cpp
      wsola.cpp
      granular.cpp
      phase_vocoder.cpp      // later
    harmony/
      harmony_engine.cpp
      scale.cpp
      midi_harmony.cpp
    dynamics/
      gate.cpp
      compressor.cpp
      deesser.cpp
    filters/
      biquad.cpp
    modulation/
      doubler.cpp
      chorus.cpp
    delay/
      stereo_delay.cpp
    reverb/
      diffuser.cpp
      fdn8.cpp
      early_reflections.cpp
    vocoder/
      vocoder.cpp             // later / AMY carrier
    platform/
      p4_simd.cpp
      profiling.cpp
  test/
    host_tests/
    esp32p4_bench/
```

## 22. Integração com AMY

Recomenda-se manter o Vocal FX como componente independente e conectá-lo ao AMY no nível do audio graph/mixer, em vez de transformar voz em um tipo de oscilador. O módulo pode compartilhar MIDI state, tempo, presets e efeitos globais, preservando independência de teste.

```text
                 +--> AMY synthesis --------+
MIDI / clock ----+                           |
                 +--> Vocal Harmony Engine   |
                                             v
MIC --> Vocal FX -----------------------> Global Mixer --> Output
                                  ^
                                  |
                         AMY carrier (vocoder)
```

## 23. Riscos técnicos e mitigação

| Risco | Sintoma | Mitigação |
|---|---|---|
| Octave errors no F0 | Harmonia pula 12 st | confidence, hysteresis, continuity model, MPM A/B |
| Pitch-mark jitter | Buzz/roughness | correlation refinement + smoothing |
| Consoantes phasey | Perda de inteligibilidade | voiced/unvoiced + dry/WSOLA crossfade |
| Formant shift | Chipmunk/monster | limitar intervalos no MVP; LPC na fase 2 |
| CPU spikes | Underrun | sem alloc/locks; profiling p99; SIMD targeted |
| PSRAM jitter | Clicks esporádicos | hot buffers internos; sequential access only |
| Reverb metálico | Modos audíveis | delay selection, diffusion, damping, modulation |
| Parameter zipper | Clicks/artefatos | smoothing por parâmetro |

## 24. Referências técnicas

- [ESP32-P4 Datasheet / Espressif](https://documentation.espressif.com/esp32-p4_datasheet_en.html)
- [ESP-DSP Library / Espressif](https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-library.html)
- [ESP-IDF I2S API / ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/i2s.html)
- [de Cheveigné & Kawahara — YIN (JASA, 2002)](https://pubmed.ncbi.nlm.nih.gov/12002874/)
- [Moulines & Charpentier — Pitch-synchronous waveform processing techniques for TTS (1990)](https://www.sciencedirect.com/science/article/pii/016763939090021Z)
- [Roelands & Verhelst — WSOLA, Eurospeech 1993](https://www.isca-archive.org/eurospeech_1993/roelands93_eurospeech.html)
- [Rubber Band — technical notes](https://www.breakfastquay.com/rubberband/technical.html)
- [Signalsmith Stretch — pitch/time DSP reference](https://signalsmith-audio.com/code/stretch/)

## 25. Brief de implementação para agente de código

O agente deve tratar este documento como especificação arquitetural, mas implementar incrementalmente. A prioridade é áudio estável e mensurável; otimização prematura deve ser evitada.

### Implementation priorities

1. Never block the real-time audio path.
2. No dynamic allocation inside `vocal_fx_process()`.
3. Build and benchmark one module at a time.
4. Keep pitch detection asynchronous from audio rendering.
5. TD-PSOLA is the default live pitch engine.
6. Preserve unvoiced consonants with dry/WSOLA fallback.
7. FDN reverb must be stable for every valid parameter value.
8. All audible parameters require smoothing.
9. Provide per-block cycle counters and underrun telemetry.
10. Optimize with ESP32-P4 intrinsics only after measurements identify hotspots.

### 25.1 Primeira entrega esperada do agente

- Componente compilável `vocal_fx` para ESP-IDF.
- Test harness host-side para biquad, YIN e FDN quando possível.
- Benchmark firmware para ESP32-P4 com cycles/block e max callback time.
- Audio passthrough 48 kHz + HPF/comp/EQ + FDN plate/hall.
- Nenhum harmonizer antes de a base RT demonstrar estabilidade.

## 26. Decisão recomendada

O ESP32-P4 é uma plataforma plausível para um vocal multi-FX musicalmente sério. A melhor relação risco/qualidade para o harmonizer inicial é YIN/MPM + TD-PSOLA + tratamento WSOLA/dry de unvoiced; o reverb deve usar FDN modulado de 8 linhas. A principal métrica de sucesso não será apenas uso de CPU, mas a robustez em transientes, mudanças rápidas de nota e ruído real de palco.

> **Próximo artefato recomendado:** a partir desta especificação, o passo mais útil é gerar o esqueleto do componente ESP-IDF (`components/vocal_fx`) com interfaces, ring buffers, profiling e um primeiro FDN funcional antes de implementar o pitch shifter.
