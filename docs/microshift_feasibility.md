# Avaliação Arquitetural de Viabilidade: Microshift no VoxP4

**Status:** Nota Técnica de Engenharia  
**Data:** 21 de Setembro de 2026  
**Contexto:** Expansão de Efeitos Vocais — Milestone Global Tempo, Delay Sync, Vocal Modulation & Drive  
**Alvo:** ESP32-P4 / Baseline TD-PSOLA Congelado  

---

## 1. O que é Microshift no contexto vocal profissional?

No ambiente de estúdio e processamento vocal ao vivo, o efeito **Microshift** (conceitualmente inspirado pelo clássico algoritmo *MicroPitch* do Eventide H3000 e por plugins contemporâneos de estúdio) é uma técnica de alargamento estéreo e adensamento tímbrico ("vocal thickening / stereo widening"). Estas referências de hardware e software servem como inspirações conceituais e alvos de design arquitetural para futuros desenvolvimentos no VoxP4, sem qualquer alegação de equivalência algorítmica direta.

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

### 4.3 Comb Filtering, Batimento Acústico e Fase
- **Física Fundamental do Batimento Acústico:** Ao somar qualquer sinal senoidal/harmônico com uma réplica transposta em frequência ligeiramente diferente ($\Delta f = f_0 \times |R - 1|$), ocorre uma modulação de amplitude periódica (batimento) a uma taxa de exatamente $\Delta f$ Hz. Por exemplo, para $f_0 = 220$ Hz e um detune de $+9$ cents ($R_+ \approx 1.0052$):
  $$\Delta f = 220 \text{ Hz} \times 0.0052 \approx 1.15 \text{ Hz}$$
  **Este batimento é um fenômeno físico inevitável de QUALQUER algoritmo** (seja analógico, delay com crossfade, phase vocoder ou TD-PSOLA) quando o sinal original é somado acusticamente ou eletricamente à sua réplica transposta. Não constitui uma falha ou deficiência específica do TD-PSOLA.
- **Onde o TD-PSOLA introduz problemas adicionais:** O que difere entre algoritmos é a presença ou ausência de descontinuidades de emenda, janelamento ou repetição de grãos. No TD-PSOLA, como os grãos são emitidos síncronos aos marcos de pitch ($T_0$), o agendamento quase síncrono para razões muito próximas de 1.0 ($R \approx 1.005$) força o algoritmo a operar em uma zona limite: por centenas de milissegundos os grãos são quase idênticos, até que ocorre o descarte ou a duplicação brusca de um grão. Além disso, quaisquer imprecisões no detector de pitch ($f_0$) ou flutuações nos pitch marks modulam erraticamente o instante de emissão, superimpondo jitter de fase e ruído de janelamento sobre o batimento acústico natural. Em contrapartida, uma linha de atraso assíncrona com velocidade linear constante mantém a progressão temporal perfeitamente contínua e suave.

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

### 5.2 Comparação Preliminar de Recursos: TD-PSOLA vs. Crossfaded Delay (Estimativas de Projeto)

> [!NOTE]
> Os valores abaixo para o Dual Crossfaded Delay representam **metas de projeto e estimativas arquiteturais preliminares**, baseadas no custo computacional de leitura linear/cúbica com rampa de ganho em blocos de 64 amostras, e NÃO medições físicas definitivas em bancada. A qualificação empírica no ESP32-P4 será conduzida quando o módulo for efetivamente desenvolvido.

| Métrica | 2x Vozes TD-PSOLA (Medido P4) | Dual Crossfaded Delay (Meta de Projeto / Estimativa) |
| :--- | :--- | :--- |
| **Consumo de Memória** | $> 150 \text{ KB}$ (PSRAM/SRAM) | $\mathbf{\approx 15 \text{ KB}}$ (meta para SRAM interna) |
| **Tempo de CPU / Bloco (64 amostras)** | $\approx 60 - 90 \ \mu\text{s}$ no P4 | $\mathbf{\approx 1.5 - 3.0 \ \mu\text{s}}$ (meta de projeto no P4) |
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

Recomenda-se implementar o Microshift em milestone futura através de **uma das duas opções arquiteturais desacopladas**:
- **Opção A (Recomendada):** Como um modo estendido do módulo `VocalChorus` existente (`ChorusMode::Microshift`), aproveitando o buffer de delay circular estéreo e a infraestrutura de interpolação já implementada.
- **Opção B:** Como módulo dedicado independente `VocalMicroshift`, inserido entre o `VocalDrive` e o `VocalChorus`.

### Justificativas Finais:
1. **Qualidade Sonora:** O algoritmo de crossfaded delay head assíncrono é uma referência conceitual clássica para micro-pitching vocal. Ele preserva o ataque dos transientes e evita os artefatos de janela ou descontinuidades inter-grãos que o TD-PSOLA produziria para $\Delta f < 15$ cents.
2. **Metas de Desempenho no ESP32-P4:** A meta de projeto de baixíssimo overhead computacional e consumo compacto de memória viabiliza o uso do efeito sem comprometer a margem de tempo real do deadline de 1451.25 µs.
3. **Segurança e Estabilidade do Código:** O baseline congelado do TD-PSOLA permanece 100% intocado, preservando todos os marcos de estabilidade conquistados no projeto VoxP4.
