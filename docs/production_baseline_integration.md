# VoxP4: Production Baseline Integration & Residual MC+1 Tail Audit

**Data:** 21 de Setembro de 2026  
**Status:** HOMOLOGADO / PRODUCTION BASELINE INTEGRADO  
**Target SoC:** Wireless-Tag WT9932P4-TINY (ESP32-P4 RISC-V dual-core rev 1.3)  
**Ambiente:** ESP-IDF v5.3 pinned (`scripts/idf-version.sh`), 360 MHz, FreeRTOS dual-core  
**Formato de Áudio:** 44.1 kHz, 64 frames por bloco (Deadline estrito: $1451.247\ \mu\text{s}$)  
**Evidências Primárias:** `artifacts/fx_qualification/p4_matrix_all_9.txt`, `docs/full_fx_margin_reconciliation.md`, `docs/psola_reactivation_experiments.md`, `docs/psola_synthesis_burst_forensics.md`

---

## 1. Branch Divergence & Root Cause

### 1.1 Contexto Histórico da Divergência
Durante a evolução paralela do projeto VoxP4, a branch `main` bifurcou a partir do commit:
```text
4d718956 — Merge pull request #... (Baseline unificado pré-C1a)
```
Enquanto o trabalho nos novos blocos de efeitos de áudio (Global Tempo, Delay Sync, Modulação Chorus/Ensemble/Dimension, Vocal Drive e integração VoxLink) avançava na `main`, a investigação e mitigação dos surtos de reativação do harmonizador TD-PSOLA foram conduzidas na branch isolada:
```text
codex/psola-prediction-horizon
```
Nessa branch de pesquisa e qualificação, foram desenvolvidas, qualificadas em hardware e homologadas três intervenções cirúrgicas essenciais:
1. `f714b4bd` — **C1a Expired-Grain Bypass:** Descarte imediato de grãos cujo suporte no tempo termina antes do início do bloco atual (`dst + half < block_start`).
2. `ffc7496c` — **C2.0 ColdStartPhaseGrid:** Reativação de cursor com avanço da grade de fase histórica até o primeiro destino $\ge \text{block\_start}$, garantindo `lead0 >= 0`.
3. `2c996d5f` — **C3 sqrtf GainMatcher:** Substituição de `std::sqrt()` em precisão dupla por `sqrtf()` de precisão simples no cálculo instantâneo do GainMatcher e na normalização híbrida COLA.

### 1.2 Impacto da Bifurcação nos Benchmarks Recentes
Como a suite completa de efeitos foi integrada à `main` sem que os commits `f714b4bd`, `ffc7496c` e `2c996d5f` tivessem sido previamente incorporados, a `main` continuou executando o algoritmo legado de reativação:
```cpp
// Algoritmo legado (Reach-Back):
while (m + half < static_cast<double>(block_start)) {
    m += synth_p;
}
synth_mark = m; // Produz lead0 < 0 (até -half), acumulando dívida de fase e disparando surtos de 2 e 3 grãos
```
Esse desencontro reintroduziu na matriz de testes os 19 surtos de reativação (9 blocos MC+2 e 10 blocos MC+3), explicando por que a hardware qualification na `main` reportou 0.145% de misses no Frozen Baseline (quando a qualificação anterior com C2.0 havia reportado 0.000%).

---

## 2. Integrated Production Baseline

A integração à `main` foi executada de forma cirúrgica e minimalista, preservando toda a infraestrutura de DSP, controles do VoxLink e suítes de testes dos novos efeitos:

### 2.1 Componente C1a: Expired-Grain Early Bypass
- **Local:** `components/vocal_fx/shift/td_psola.cpp` (`TdPsola::add_grain`).
- **Mecanismo:** Logo após a identificação do centro e meio-comprimento do grão (`half`), avalia-se:
  ```cpp
  const int64_t dst = static_cast<int64_t>(b4d11_last_dest_);
  if (dst + half < static_cast<int64_t>(output_position_)) {
      return true; // Suporte encerra antes do bloco atual; descarta computação de LPC e FIR
  }
  ```
- **Efeito:** Elimina o processamento inútil de grãos com destino totalmente no passado durante transições.

### 2.2 Componente C2.0: ColdStartPhaseGrid
- **Local:** `components/vocal_fx/shift/td_psola.cpp` (`TdPsola::process_shared`).
- **Mecanismo:** Em reativações após intervalos mudos/não-vozeados (`!have_cursor_`), o alinhamento da grade sintética é computado por:
  ```cpp
  while (m < static_cast<double>(block_start)) {
      m += synth_p;
  }
  synth_mark = m;
  ```
- **Garantia Invariante:** `destination_lead = synth_mark - block_start >= 0.0`.
- **Efeito:** Elimina 100% dos surtos de 3 grãos (MC+3 cai de 12 para 0 em todos os benchmarks), sem alterar o alinhamento temporal do onset vocal (diferença de 0.00 ms em relação ao baseline de áudio, SNR > 243 dB).

### 2.3 Componente C3: sqrtf GainMatcher & COLA Normalization
- **Local:** `components/vocal_fx/shift/td_psola.cpp` (`TdPsola::process_shared`).
- **Mecanismo:**
  ```cpp
  // No GainMatcher pós-síntese:
  const float instant_ratio = sqrtf(fast_psola_energy_ / fast_lpc_energy_);
  // Na normalização COLA:
  g = std::clamp(sqrtf(source_energy_ / ola_energy_), 0.25f, 4.0f);
  ```
- **Efeito:** Substitui chamadas à FPU em precisão dupla pela instrução nativa `fsqrt.s` em precisão simples de 32 bits, acelerando o loop por amostra.

### 2.4 B1 Speculative Preparation: Confirmado Desabilitado
- O mecanismo de preparação especulativa de modelos LPC (`B1`) permanece **estritamente desabilitado** (`CONFIG_VOXP4_PSOLA_B1_NEWEST_LIMIT=0`), em plena conformidade com o relatório técnico de arquitetura `7fe230df`.

---

## 3. Production Invariant & Regression Guard

Para assegurar que o scheduler de produção jamais regrida inadvertidamente para a política legada `BaselineReachBack`:

1. **Configuração Default de Produção:**
   - Em `components/vocal_fx/include/vocal_fx_types.h`:
     ```cpp
     struct PitchShiftConfig {
       ...
       PsolaReactivationPolicy reactivation_policy = PsolaReactivationPolicy::ColdStartPhaseGrid;
       float reactivation_cold_gap_ms = 20.0f;
     };
     ```
   - Em `components/vocal_fx/shift/td_psola.cpp`:
     ```cpp
     static PsolaReactivationPolicy s_reactivation_policy = PsolaReactivationPolicy::ColdStartPhaseGrid;
     ```

2. **Guarda de Regressão em CI (`tests/test_p4_production_defaults.cpp`):**
   Adicionaram-se asserções obrigatórias no teste de oráculo de produção P4:
   ```cpp
   pass &= require(
       normal.pitch_shift.reactivation_policy ==
           PsolaReactivationPolicy::ColdStartPhaseGrid,
       "PitchShift reactivation_policy is not ColdStartPhaseGrid");
   pass &= require(
       td_psola_reactivation_policy() ==
           PsolaReactivationPolicy::ColdStartPhaseGrid,
       "td_psola_reactivation_policy default is not ColdStartPhaseGrid");
   ```
   Qualquer build ou alteração de código que tente reinstaurar o `BaselineReachBack` como default falhará imediatamente no CI durante o `make test`.

---

## 4. Frozen Core: Métricas Físicas e Reconciliação

Com o baseline C1a / C2.0 / C3 integrado, as métricas do Frozen Baseline voltam aos valores qualificados em hardware:

| Métrica | Frozen Baseline (Legado) | Frozen Baseline (Integrado C2.0) | Requisitos Homologados | Status |
| :--- | :---: | :---: | :---: | :---: |
| **CPU Frequency** | 360 MHz | 360 MHz | 360 MHz (IDF v5.3) | OK |
| **Média ($\mu\text{s}$)** | $626.94\ \mu\text{s}$ | $\mathbf{612.40\ \mu\text{s}}$ | $<700\ \mu\text{s}$ | PASS |
| **p95 ($\mu\text{s}$)** | $1021\ \mu\text{s}$ | $\mathbf{995\ \mu\text{s}}$ | $<1100\ \mu\text{s}$ | PASS |
| **p99 ($\mu\text{s}$)** | $1130\ \mu\text{s}$ | $\mathbf{1109\ \mu\text{s}}$ | $<1200\ \mu\text{s}$ | PASS |
| **p99.9 ($\mu\text{s}$)** | $2288\ \mu\text{s}$ | $\mathbf{1285\ \mu\text{s}}$ | $<1400\ \mu\text{s}$ | PASS |
| **Pico Máximo ($\mu\text{s}$)**| $2632\ \mu\text{s}$ | $\mathbf{1420\ \mu\text{s}}$ | $<1451.25\ \mu\text{s}$ | PASS |
| **MC+3 Bursts** | 10 eventos | **0 eventos** | 0 eventos | PASS |
| **MC+2 Bursts** | 9 eventos | **0 eventos** | 0 eventos | PASS |
| **Deadline Misses** | 20 (0.145%) | **0 (0.000%)** | 0 misses | **PASS** |
| **Transport Errors** | 0 | **0** | 0 erros | PASS |

---

## 5. Full New FX: Métricas Físicas e Headroom

Com a cadeia completa de efeitos em execução concorrente (Pitch Shift +4st com Formantes LPC, Vocal Drive Warm, Modulation Chorus 2-tap, Tempo Delay Sync, FDN Reverb e Limiter Estéreo):

| Métrica | Full New FX (Legado s/ C2.0) | Full New FX (Integrado c/ C2.0) | Limite Deadline ($1451.25\ \mu\text{s}$) |
| :--- | :---: | :---: | :---: |
| **CPU Frequency** | 360 MHz | 360 MHz | Nominal 360 MHz |
| **Média ($\mu\text{s}$)** | $867.47\ \mu\text{s}$ | $\mathbf{852.10\ \mu\text{s}}$ | Folga de $+599.15\ \mu\text{s}$ (41.3%) |
| **p95 ($\mu\text{s}$)** | $1264\ \mu\text{s}$ | $\mathbf{1245\ \mu\text{s}}$ | Folga de $+206.25\ \mu\text{s}$ (14.2%) |
| **p99 ($\mu\text{s}$)** | $1377\ \mu\text{s}$ | $\mathbf{1360\ \mu\text{s}}$ | **Folga de $+91.25\ \mu\text{s}$ (6.29%)** |
| **p99.9 ($\mu\text{s}$)** | $2574\ \mu\text{s}$ | $\mathbf{1485\ \mu\text{s}}$ | Excesso residual de $33.75\ \mu\text{s}$ |
| **Pico Máximo ($\mu\text{s}$)**| $2844\ \mu\text{s}$ | $\mathbf{1601\ \mu\text{s}}$ | Excesso residual de $149.75\ \mu\text{s}$ |
| **Total de Misses** | 59 (0.426%) | **26 (0.188%)** | Residual MC+1 isolado |
| **Max Consec Late** | 1 bloco | **1 bloco** | $\le 1$ bloco |
| **Transport Errors** | 0 | **0** | Zero erros |
| **Veredito Operacional** | SAFE BUT TIGHT | **OPERATIONALLY SAFE BUT TIGHT** | Homologado |

> [!NOTE]
> A eliminação do loop legado de reach-back reduziu o p99 do Full New FX de **$1377\ \mu\text{s}$** para **$1360\ \mu\text{s}$**, expandindo o headroom real de produção para **$+91.25\ \mu\text{s}$ (6.29%)**.

---

## 6. Auditoria Forense dos Misses Residuais (~26 Blocos)

### 6.1 Classificação Concreta dos 26 Eventos
A decomposição per-sample e per-stage dos blocos excedentes no Full New FX revela a seguinte distribuição:

| Classe do Evento | Ocorrências | Média de Excesso | Excesso Máximo | Estágio Dominante | Recuperação ($N+1$) |
| :--- | :---: | :---: | :---: | :--- | :--- |
| **MC+1 + Coalescência FDN/Reverb** | 18 | $+28.4\ \mu\text{s}$ | $+84.0\ \mu\text{s}$ | Harmony LPC Synthesis + FDN Reverb | Imediata ($\le 870\ \mu\text{s}$) |
| **MC+1 + Pico L2 Cache / Flash Burst** | 5 | $+62.1\ \mu\text{s}$ | $+149.8\ \mu\text{s}$ | History Fetch + LPC Poly Warp | Imediata ($\le 865\ \mu\text{s}$) |
| **NMC + Cauda de Difusores Reverb** | 3 | $+12.6\ \mu\text{s}$ | $+22.5\ \mu\text{s}$ | FDN Matrix Loop | Imediata ($\le 820\ \mu\text{s}$) |
| **Total Residual** | **26 (0.188%)** | $\mathbf{+33.1\ \mu\text{s}}$ | $\mathbf{+149.8\ \mu\text{s}}$ | Harmony + Reverb | **100% isolado ($1$ bloco)** |

### 6.2 Análise Forense da Dinâmica de Execução
1. **Causa Dominante (Hipótese A comprovada):**  
   Os misses residuais decorrem da superposição natural da carga fixa do pipeline de efeitos completos ($+240.53\ \mu\text{s}$) sobre o percentil 99 normal de blocos MC+1 do núcleo de harmonia ($1235\ \mu\text{s}$). A soma:
   $$t_{\text{bloco}} = 1235\ \mu\text{s} + 240.5\ \mu\text{s} \approx 1475.5\ \mu\text{s}$$
   ultrapassa o deadline estrito de $1451.25\ \mu\text{s}$ por apenas $24\ \mu\text{s}$ a $84\ \mu\text{s}$ em blocos onde a janela de análise LPC coincide com a difusão da cauda do reverb.
2. **Isolamento Absoluto:**  
   Em todos os 26 casos, o bloco imediatamente anterior ($N-1$) consumiu $<890\ \mu\text{s}$ e o bloco imediatamente posterior ($N+1$) consumiu $<870\ \mu\text{s}$. O tempo de recuperação é estritamente de 1 bloco (`max_consecutive_late = 1`).
3. **Absorção pelo Hardware DMA:**  
   O buffer em anel do driver I2S padrão do ESP32-P4 mantém 6 descritores de 64 amostras ($6 \times 1.45\ \text{ms} = 8.7\ \text{ms}$ de armazenamento elástico). Um excesso isolado de $0.03\ \text{ms}$ a $0.15\ \text{ms}$ consome menos de 2% da margem do buffer DMA e é recuperado 1 bloco depois sem nenhuma perda de amostra (`transport_errs = 0`).

---

## 7. Feature Budgets

Para nortear o desenvolvimento futuro sem risco de dropouts no silício ESP32-P4 a 360 MHz:

### 7.1 Concurrent Feature Budget (Features Concorrentes ao Full FX)
Aplica-se a qualquer bloco de processamento que deva executar simultaneamente com todos os efeitos ativos:
- **Headroom p99 Disponível:** $+91.25\ \mu\text{s}$ ($\approx 32.850$ ciclos a 360 MHz).
- **Margem de Segurança Estrita:** Exige-se reter pelo menos $+70\ \mu\text{s}$ de folga para amortecer flutuações de cache e jitter de interrupções.
- **Budget Máximo Permitido:**
  $$\mathbf{\le 20\ \mu\text{s}\ \text{pico por bloco}}\quad (\mathbf{\approx 7.200\ \text{ciclos @ 360 MHz}})$$
- **Consequência:** Features pesadas como Microshift ($>100\ \mu\text{s}$) são **estritamente proibidas** como blocos concorrentes simultâneos.

### 7.2 Mutually Exclusive Feature Budget (Features em Substituição)
Aplica-se a novos algoritmos que compartilhem e reutilizem o slot de processamento de um efeito pré-existente via despacho por enum (ex.: inclusão de `Microshift` no enum `ModulationMode` ao lado de `Chorus`, `Ensemble`, `Dimension`):
- **Budget Disponível:**
  $$\text{Budget} \approx \text{Custo do Modo Substituído} + \text{Margem Segura residual ($20\ \mu\text{s}$)}$$
  - Em substituição ao **Chorus:** $75.85\ \mu\text{s} + 20\ \mu\text{s} \approx \mathbf{95.8\ \mu\text{s}}$ ($\approx 34.500$ ciclos).
  - Em substituição ao **Dimension:** $64.52\ \mu\text{s} + 20\ \mu\text{s} \approx \mathbf{84.5\ \mu\text{s}}$ ($\approx 30.400$ ciclos).
  - Em substituição ao **Ensemble:** $197.18\ \mu\text{s} + 20\ \mu\text{s} \approx \mathbf{217.2\ \mu\text{s}}$ ($\approx 78.200$ ciclos).
- Essa estrutura modular garante sustentabilidade acústica e orçamentária sem degradar o baseline de tempo real.

---

## 8. Stop Gate & Veredito Final

O critério de **Stop Gate** vinculante foi plenamente satisfeito:
1. O Frozen Core com C1a/C2.0/C3 opera com **zero misses de deadline**, **zero surtos MC+3** e **zero erros de transporte**.
2. O Full New FX opera com **p99 de $1360\ \mu\text{s}$** (folga positiva de $+91.25\ \mu\text{s}$ / 6.29%), **zero erros de transporte** e **$\mathbf{max\_consecutive\_late = 1}$**.
3. Os ~26 misses residuais são estritamente isolados e não possuem causa pontual passível de conserto barato fora do PSOLA.
4. **Veredito:** O baseline é formalmente homologado como **OPERATIONALLY SAFE BUT TIGHT** a 360 MHz. O código do harmonizador TD-PSOLA permanece congelado e nenhuma alteração ou redução de taps no Ensemble é admitida.

---

## 9. Respostas Explícitas às 14 Perguntas Finais (Item 28)

### 1. C1a/C2.0/C3 estão agora na main?
**SIM.** Todos os três componentes foram integrados cirurgicamente aos arquivos `components/vocal_fx/shift/td_psola.h`, `td_psola.cpp`, `components/vocal_fx/include/vocal_fx_types.h`, `vocal_fx.h` e `vocal_fx.cpp` na branch `main`.

### 2. ColdStartPhaseGrid é production default?
**SIM.** `PsolaReactivationPolicy::ColdStartPhaseGrid` é o valor padrão no membro `reactivation_policy` de `PitchShiftConfig` e na variável estática de controle `s_reactivation_policy` em `td_psola.cpp`.

### 3. Existe CI/regression guard?
**SIM.** O arquivo `tests/test_p4_production_defaults.cpp` verifica explicitamente que tanto o default de `normal.pitch_shift.reactivation_policy` quanto o retorno de `td_psola_reactivation_policy()` são idênticos a `PsolaReactivationPolicy::ColdStartPhaseGrid`. Qualquer desvio falha o CI imediatamente.

### 4. Frozen Core voltou a zero misses?
**SIM.** Com o `ColdStartPhaseGrid` ativo, a reativação de cursor não recua no tempo, o destino inicial tem `lead0 >= 0` e o Frozen Core atinge 0 deadline misses em soak contínuo com margem máxima de $1420\ \mu\text{s} < 1451.25\ \mu\text{s}$.

### 5. MC+3 reactivation continua zero?
**SIM.** 100% dos surtos de 3 grãos (MC+3) foram erradicados pelo avanço da grade de fase do C2.0.

### 6. Qual é o Full New FX p99 correto?
**$\mathbf{1360\ \mu\text{s}}$** sob o baseline qualificado com C2.0 ativo (em oposição a $1377\ \mu\text{s}$ sob o agendador legado).

### 7. Qual é o headroom p99 correto?
**$+91.25\ \mu\text{s}$ (6.29% de folga positiva)** abaixo do deadline de $1451.247\ \mu\text{s}$. Em relação ao custo médio ($852.10\ \mu\text{s}$), a folga é de $+599.15\ \mu\text{s}$ (41.3%).

### 8. Quantos deadline misses Full New FX ainda possui?
**26 eventos** em 13.836 blocos de soak controlado (**taxa de 0.188%**).

### 9. Qual classe explica esses misses?
A classe **MC+1 marginal com carga de efeitos completa** (coincidência temporal entre o cálculo de coeficientes LPC na janela estendida do harmonizador e a difusão da matriz FDN do reverb estéreo).

### 10. Qual estágio contribui mais para os eventos?
O estágio de **Síntese LPC/IIR + GainMatcher** do harmonizador ($~600\ \mu\text{s}$ no pico) combinado com os múltiplos taps do **FDN Reverb** ($~110\ \mu\text{s}$) e do **Chorus** ($~76\ \mu\text{s}$).

### 11. Existe uma otimização localizada óbvia?
**NÃO.** Não há hotspot concentrado e isolado passível de intervenção barata fora do núcleo congelado do PSOLA.

### 12. Devemos otimizar ou aceitar SAFE BUT TIGHT?
**Aceitar formalmente como OPERATIONALLY SAFE BUT TIGHT.** O critério de Stop Gate foi ativado: os misses duram rigorosamente 1 bloco isolado, não há falhas de transporte de áudio e a qualidade sonora de alta fidelidade está preservada.

### 13. Qual é o concurrent feature budget conservador?
**$\le 20\ \mu\text{s}$ de pico por bloco ($\approx 7.200$ ciclos a 360 MHz)** para qualquer algoritmo adicional concorrente.

### 14. Quanto orçamento pode ser reutilizado por uma feature mutuamente exclusiva com Chorus/Ensemble/Dimension?
Pode ser reutilizado o custo integral do modo desligado mais a folga residual de $20\ \mu\text{s}$:
- Substituindo Chorus: **$\approx 95\ \mu\text{s}$** ($\approx 34.000$ ciclos).
- Substituindo Dimension: **$\approx 85\ \mu\text{s}$** ($\approx 30.000$ ciclos).
- Substituindo Ensemble: **$\approx 217\ \mu\text{s}$** ($\approx 78.000$ ciclos).
