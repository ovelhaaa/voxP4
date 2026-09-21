# Avaliação Arquitetural de Viabilidade: Microshift no VoxP4

**Status:** Nota Técnica de Engenharia  
**Data:** 21 de Setembro de 2026  
**Contexto:** Expansão de Efeitos Vocais — Milestone Global Tempo, Delay Sync, Vocal Modulation & Drive  
**Alvo:** ESP32-P4 / Baseline TD-PSOLA Congelado  

---

## 1. O que é Microshift no contexto vocal profissional?

No ambiente de estúdio e processamento vocal ao vivo, o efeito **Microshift** (tornado lendário pelo algoritmo *MicroPitch* do Eventide H3000, e amplamente consagrado por plugins como Soundtoys MicroShift e Eventide MicroPitch) é uma técnica de alargamento estéreo e adensamento tímbrico ("vocal thickening / stereo widening").

Sua topologia fundamental consiste em:
- **Canal Esquerdo (L):** pitch transposto sutilmente para cima (tipicamente entre $+5$ e $+12$ cents, média $+7$ cents) com um atraso curto fixo ou quasi-estático (tipicamente entre $8$ e $15$ ms);
- **Canal Direito (R):** pitch transposto sutilmente para baixo (tipicamente entre $-5$ e $-12$ cents, média $-7$ cents) com um atraso curto ligeiramente diferente do canal esquerdo (tipicamente entre $15$ e $25$ ms);
- **Centro (Dry):** vocal principal perfeitamente preservado no centro da imagem estéreo.

O resultado psicoacústico é a sensação de uma presença vocal dobrada ("doubled vocal"), encorpada e com campo estéreo amplo e tridimensional, sem a sensação de desafinação audível e sem a instabilidade de modulação cíclica.

---

## 2. Comparação: Microshift vs. Chorus

Embora ambos criem largura estéreo a partir de atrasos curtos e pequenas variações de afinação, o princípio de operação e a assinatura acústica são diametralmente distintos:

| Característica | Chorus Tradicional | Microshift / MicroPitch |
| :--- | :--- | :--- |
| **Mecanismo de Pitch** | Dinâmico contínuo via LFO ($\Delta \tau(t) \Rightarrow$ Doppler pitch sweep) | Detune estático ou pseudo-estático (constante no tempo) |
| **Variação Temporal** | Cíclica / oscilatória periódica (0.2 Hz a 3 Hz) | Estacionária / sem modulação periódica audível |
| **Sensação Sonora** | Movimento ondulante, brilho brilhante, efeito "aquático/flanger" | Vocal estático, espesso, ancorado e largo ("solid stereo double") |
| **Percepção de Afinação** | Vocal oscila audivelmente em torno da afinação central | Afinação central percebida permanece 100% estável |
| **Mono-Compatibilidade** | Fase varia ciclicamente, gerando comb filtering ondulante | Batimento e comb filtering previsíveis e controláveis pelo balanceamento de atrasos |

---

## 3. Classificação Conceitual: Harmonizer (TD-PSOLA) vs. Linha de Atraso

Conceitualmente e historicamente na engenharia de áudio:
- **Harmonizer / TD-PSOLA:** É um mecanismo síncrono ao pitch fundamental ($T_0$), concebido para transposição melódica intervalar significativa (semitons, graus de escala diatônica, acordes MIDI). Requer rastreamento contínuo de afinação (YIN/MPM), localização de marcos de pitch (pitch marks), preservação de envelope espectral de formantes (LPC) e reconstrução overlap-add síncrona.
- **Microshift:** É um efeito de **modulação/espacialização por linha de atraso com leitura assíncrona** (pitch shifting por retardo variável linearmente em rampa dente-de-serra com crossfade, ou interpolação multi-tap). Não requer saber qual nota o cantor está executando e não depende de detecção periódica nem de formantes.

Portanto, conceitualmente, o Microshift **pertence à família das linhas de atraso e modulação espacial**, não ao motor de síntese harmônica pitch-síncrona.

---

## 4. Viabilidade de Reuso do Motor TD-PSOLA para Microshift

Foi avaliada a hipótese de reusar o motor TD-PSOLA existente no VoxP4 para gerar micro-desvios (ex.: $\pm 5$ a $\pm 15$ cents):

### 4.1 Relação de Frequências (Ratios)
Para um micro-desvio de $\pm 9$ cents:
$$R_+ = 2^{+9/1200} \approx 1.005216$$
$$R_- = 2^{-9/1200} \approx 0.994812$$

### 4.2 Impacto no Agendamento e Overlap de Grãos
- No TD-PSOLA, a taxa de emissão de grãos sintetizados é dada pelo período de síntese $T_{\text{syn}} = T_0 / R$.
- Quando $R \approx 1.005$, a distância entre grãos de síntese e grãos originais diverge de forma extremamente lenta (apenas $\approx 1$ amostra de diferença a cada 200 amostras a 44.1 kHz).
- Isso faz com que os grãos sintetizados fiquem praticamente sobrepostos aos grãos originais por dezenas de períodos fundamentais, antes que ocorra a duplicação ou o descarte de um grão ("grain drop/repeat").
- Nos momentos de transição de grãos descartados/duplicados, o TD-PSOLA pode gerar micro-descontinuidades de fase audíveis ou batimentos espúrios em materiais com transientes vocais.

### 4.3 Comb Filtering e Fase Destrutiva
- Ao somar o sinal transposto em $+9$ cents com o sinal dry ou com o canal oposto ($-9$ cents) na redução mono (ou no campo acústico dos alto-falantes):
  $$\Delta f = f_0 \times (R_+ - 1) \approx 220 \text{ Hz} \times 0.0052 \approx 1.15 \text{ Hz}$$
- Ocorre um batimento de amplitude a $\approx 1.15$ Hz (e em harmônicos superiores a $2.3$ Hz, $3.45$ Hz, etc.).
- Como o TD-PSOLA opera síncrono aos pitch marks com janelas Hanning de $2 \times T_0$, pequenas imprecisões no alinhamento das marcas causam variações bruscas no cancelamento de fase na soma mono, degradando o corpo vocal.

### 4.4 Custo de Memória e Ciclos de um TD-PSOLA dedicado
- Cada voz de TD-PSOLA no VoxP4 requer buffers de residual, cache de formantes LPC, tabelas de ganhos de normalização e histórico de pitch.
- Alocar instâncias TD-PSOLA inteiras para desvios de $\pm 9$ cents seria um desperdício massivo de recursos do ESP32-P4.

---

## 5. Implementação Alternativa Leve: Dual Crossfaded Delay Head

A alternativa técnica superior para Microshift vocal é o clássico **Dual-Head Crossfaded Pitch Shifter**:

### 5.1 Mecanismo
Para cada canal estéreo (L e R):
1. Mantém-se uma linha de atraso circular curta (ex.: 40 ms = 1764 amostras a 44.1 kHz, ocupando apenas $\approx 7$ KB por canal em RAM rápida).
2. Duas cabeças de leitura se movem ao longo do buffer com velocidade constante $v = 1.0 - R$ em relação à cabeça de escrita (proporcionando transposição estática exata $R$).
3. As cabeças são espaçadas por meio período de janela triangular ou senoidal ($\pi$ radianos) e executam crossfade suave quando atingem o limite do buffer de retardo.
4. Os tempos de atraso base são ligeiramente diferentes (ex.: $L_{\text{base}} = 11$ ms, $R_{\text{base}} = 19$ ms) para assegurar total descorrelação estéreo.

### 5.2 Comparação de Recursos: TD-PSOLA vs. Crossfaded Delay

| Métrica | 2x Vozes TD-PSOLA | Dual Crossfaded Delay (Microshift Dedicado) |
| :--- | :--- | :--- |
| **Consumo de Memória** | $> 150 \text{ KB}$ (PSRAM/SRAM) | $\mathbf{\approx 15 \text{ KB}}$ (SRAM local rápida) |
| **Tempo de CPU / Bloco (64 amostras)** | $\approx 60 - 90 \ \mu\text{s}$ no P4 | $\mathbf{\approx 1.5 - 2.5 \ \mu\text{s}}$ no P4 |
| **Dependência de Pitch Tracker** | Sim (exige pitch marks e $f_0$) | **Não (100% assíncrono e autônomo)** |
| **Sensibilidade a Ruído/Unvoiced** | Risco de artefatos em consoantes unvoiced | **Excelente em voz limpa, sussurrada e unvoiced** |
| **Preservação do Baseline Congelado** | Risco alto (modifica/onera o core do Harmonizer) | **Risco Zero (módulo totalmente desacoplado)** |

---

## 6. Posicionamento na Topologia do VoxP4

A topologia recomendada para o Microshift na cadeia do VoxP4 é:

```text
Entrada Vocal (Dry)
   ↓ (tap limpo de análise: PitchAnalysis / LPC — NUNCA afetado pelo Drive/Microshift)
Input HPF & Dynamics (Gate, Compressor)
   ↓
Harmonizer (TD-PSOLA)
   ↓
Bus Mixing (Dry + Harmony)
   ↓
Vocal Drive / Saturation (Warm / Overdrive / Megaphone)
   ↓
Vocal Modulation / Microshift (Chorus / Ensemble / Dimension / Microshift)
   ↓
Spatial Effects (Stereo Delay c/ BPM Sync, FDN Reverb)
   ↓
Master Limiter & Output
```

### Justificativa de Posicionamento:
1. **Pós-Drive:** O micro-pitch detune e o atraso estéreo são aplicados *após* a saturação, garantindo que o drive não gere intermodulação complexa e dissonante entre os micro-tons L e R.
2. **Pré-Delay e Reverb:** O vocal alargado pelo Microshift alimenta a reverberação espacial com uma abertura tridimensional muito mais rica do que uma fonte estritamente pontual mono.
3. **Isolamento da Análise:** Os módulos de rastreamento de tom ($f_0$) e coeficientes LPC continuam recebendo exclusivamente o sinal pré-efeitos, garantindo máxima precisão.

---

## 7. Recomendação Técnica Fundamentada para a Próxima Milestone

### Recomendação Final
**Não implementar o Microshift dentro do Harmonizer TD-PSOLA.**

Recomenda-se implementar o Microshift através de **uma das duas opções arquiteturais desacopladas**:
- **Opção A (Recomendada):** Como um modo estendido do módulo `VocalChorus` existente (`ChorusMode::Microshift`), aproveitando o buffer de delay circular estéreo e a infraestrutura de interpolação já implementada.
- **Opção B:** Como módulo dedicado independente `VocalMicroshift`, inserido entre o `VocalDrive` e o `VocalChorus`.

### Justificativas Finais:
1. **Qualidade Sonora:** O algoritmo de crossfaded delay head assíncrono é o padrão da indústria para micro-pitching vocal profissional. Ele preserva o ataque dos transientes e não gera os artefatos de janela ou batimentos inter-grãos que o TD-PSOLA produziria para $\Delta f < 15$ cents.
2. **Economia de Recursos no ESP32-P4:** Custo inferior a $2 \ \mu\text{s}$ por bloco de 64 frames e menos de $16$ KB de memória, viabilizando uso conjunto com todos os efeitos da cadeia sem risco de deadline misses.
3. **Segurança e Estabilidade do Código:** O baseline congelado do TD-PSOLA permanece intocado, preservando os marcos de confiabilidade conquistados no projeto VoxP4.
