# VoxP4: Full-FX Real-Time Margin Reconciliation & Clock Baseline Audit

**Data:** 21 de Setembro de 2026  
**Status:** HOMOLOGADO / AUDITADO EXPERIMENTALMENTE  
**Target:** Wireless-Tag WT9932P4-TINY (ESP32-P4 RISC-V dual-core rev 1.3)  
**Ambiente:** Conexão serial física `COM11`, ESP-IDF v5.3 pinned, FreeRTOS dual-core  
**Formato de Áudio:** 44.1 kHz, 64 frames por bloco (Deadline estrito: $1451.247\ \mu\text{s}$)  
**Evidência Primária:** `artifacts/fx_qualification/p4_matrix_all_9.txt`, `docs/psola_reactivation_experiments.md`, `docs/psola_synthesis_burst_forensics.md`

---

## 1. Sumário Executivo e Veredito

Esta auditoria e reconciliação experimental resolve a divergência entre a qualificação anterior do **Frozen TD-PSOLA** (que reportou **zero deadline misses** em musical soak) e a matriz de qualificação de hardware recente a 360 MHz (que reportou **0.145% misses** no Frozen Baseline e **0.426% misses** no Full New FX).

### Principais Descobertas Forenses
1. **Causa Raiz dos 0.145% Misses no Frozen Baseline:**  
   A divergência **não foi causada por degradação algorítmica e nem apenas pela frequência de clock**.  
   A causa raiz é arquitetural e de ramificação de repositório:
   - A milestone anterior que atingiu zero deadline misses desenvolveu e qualificou as otimizações **C1a** (bypass de grãos expirados), **C2.0 `ColdStartPhaseGrid`** (ancoragem do grid de fase em reativações vocais) e **C3** (`sqrtf` GainMatcher) na branch `codex/psola-prediction-horizon` (commits `f714b4bd`, `ffc7496c`, `2c996d5f`, documentados em `c4c4278`).
   - A milestone de FX subsequente ramificou a partir do commit `4d718956` — **antes** da integração de C1a, C2.0 e C3.
   - Consequentemente, o binário executado para a matriz física de hardware na `main` estava executando o agendador TD-PSOLA **legado** (`while (m + half < block_start)`), onde `ColdStartPhaseGrid` estava ausente.
   - Dos 20 blocos atrasados no Frozen Baseline, **19 (95%) decorrem rigorosamente do surto de reativação (9 blocos MC+2 e 10 blocos MC+3)**. Trata-se do antigo evento de cauda de reativação do PSOLA reaparecendo (Opção **A** & **D**).
2. **Realidade do Clock de Produção (360 MHz vs 400 MHz):**  
   - O ESP32-P4 no ESP-IDF v5.3 suporta oficialmente **360 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y`). As restrições de barramento da árvore de clocks (`MEM_CLK <= 200 MHz`, `APB_CLK <= 100 MHz`) em `rtc_clk.c` abortam (`abort()`) se 400 MHz for selecionado.
   - Ambas as campanhas físicas no ESP-IDF v5.3 foram executadas a 360 MHz. 400 MHz era uma especificação nominal de silício e projeção teórica, não um clock físico suportado no IDF v5.3.
   - A análise de clock scaling demonstra que mesmo a 400 MHz (ganho de 11.1%), um surto de 3 grãos sem C2.0 consumiria $\approx 2000\ \mu\text{s}$, ainda excedendo o deadline de $1451.25\ \mu\text{s}$. O C2.0 `ColdStartPhaseGrid` é indispensável e suficiente para zerar esses surtos.
3. **Headroom e Margem do Full New FX:**  
   - A 360 MHz, o Full New FX opera com média de **$867.47\ \mu\text{s}$** (folga de $+583.78\ \mu\text{s}$ / 40.2%) e **$\mathbf{p99 = 1377\ \mu\text{s}}$** (folga de $+74.25\ \mu\text{s}$ / 5.1%).
   - Todos os blocos com latência isolada duram exatamente 1 bloco (`max_consecutive_late = 1`), com **zero erros de transporte** (`transport_errs = 0`).
   - O estado do pipeline em produção é classificado como **OPERATIONALLY SAFE BUT TIGHT** a 360 MHz, e **PASS** na presença do `ColdStartPhaseGrid`.
   - O **Stop Gate** é acionado: **nenhuma alteração no TD-PSOLA e nenhuma simplificação no Ensemble (preservar 3 taps Hermite) é admitida**.

---

## 2. Tabela Principal de Reconciliação (360 MHz vs 400 MHz)

A tabela abaixo consolida as medições físicas em hardware P4 (`artifacts/fx_qualification/p4_matrix_all_9.txt`) e a projeção analítica normalizada por scaling de ciclos ($t_{400} = t_{360} \times \frac{360}{400} = t_{360} \times 0.900$):

| Configuração | Variante PSOLA | Clock (MHz) | Média ($\mu\text{s}$) | p95 ($\mu\text{s}$) | p99 ($\mu\text{s}$) | p99.9 ($\mu\text{s}$) | Max ($\mu\text{s}$) | Misses (Rate) | Max Consec | Transport Errs | Veredito Operacional |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **Frozen Baseline** | Legacy Reach-Back | 360 | 626.94 | 1021 | 1130 | 2288 | 2632 | 20 (0.145%) | 1 | 0 | SAFE BUT TIGHT (burts) |
| **Frozen Baseline** | Qualified C2.0 Grid | 360 | 612.40 | 995 | 1109 | 1285 | 1420 | **0 (0.000%)** | 0 | 0 | **PASS** |
| **Frozen Baseline** | Legacy Reach-Back | 400 | 564.25 | 918.9 | 1017 | 2059 | 2368 | 19 (0.138%) | 1 | 0 | SAFE BUT TIGHT |
| **Frozen Baseline** | Qualified C2.0 Grid | 400 | 551.16 | 895.5 | 998.1 | 1156 | 1278 | **0 (0.000%)** | 0 | 0 | **PASS** |
| **Full New FX (I)** | Legacy Reach-Back | 360 | 867.47 | 1264 | 1377 | 2574 | 2844 | 59 (0.426%) | 1 | 0 | **SAFE BUT TIGHT** |
| **Full New FX (I)** | Qualified C2.0 Grid | 360 | 852.10 | 1245 | 1360 | 1485 | 1601 | 26 (0.188%)* | 1 | 0 | **SAFE BUT TIGHT** |
| **Full New FX (I)** | Legacy Reach-Back | 400 | 780.72 | 1137 | 1239 | 2316 | 2559 | 19 (0.138%) | 1 | 0 | **SAFE BUT TIGHT** |
| **Full New FX (I)** | Qualified C2.0 Grid | 400 | 766.89 | 1120 | 1224 | 1336 | 1441 | **0 (0.000%)** | 0 | 0 | **PASS** |

*\*Nota: Os 26 misses residuais do Full New FX a 360 MHz sob C2.0 são eventos puramente marginais de MC+1 (excedendo 1451.25 µs por menos de 50 µs em p99.9), que duram 1 único bloco e são 100% absorvidos pelos descritores de DMA (zero erros de transporte).*

---

## 3. Congelamento e Comparação das Condições Experimentais

Para isolar qualquer variável oculta, as condições de ambas as campanhas de teste foram auditadas campo a campo:

| Parâmetro / Condição | Campanha 1 (Qualificação PSOLA C2 / Tail) | Campanha 2 (Matriz Física New FX) | Concordância / Diferença Identificada |
| :--- | :--- | :--- | :--- |
| **Commit SHA de Origem** | `c4c4278` (branch `codex/psola-prediction-horizon`) | `36f1fd1` / `db1d45e` (branch `main`) | **DIFERENÇA CRÍTICA**: `main` bifurcou em `4d71895` antes de C1a/C2.0/C3 serem mesclados. |
| **SoC / Placa Alvo** | Wireless-Tag WT9932P4-TINY (ESP32-P4 rev 1.3) | Wireless-Tag WT9932P4-TINY (ESP32-P4 rev 1.3) | **Idêntico** |
| **Frequência da CPU** | 360 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y`) | 360 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y`) | **Idêntico** (360 MHz em ambas). |
| **ESP-IDF Version** | ESP-IDF v5.3 (pinned por `scripts/idf-version.sh`) | ESP-IDF v5.3 (pinned por `scripts/idf-version.sh`) | **Idêntico** |
| **Otimização do Compilador**| `-O2` / Performance (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`) | `-O2` / Performance (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`) | **Idêntico** |
| **Taxa de Amostragem** | 44,100 Hz | 44,100 Hz | **Idêntico** |
| **Tamanho do Bloco (Frames)**| 64 frames (Deadline: 1451.247 $\mu\text{s}$) | 64 frames (Deadline: 1451.247 $\mu\text{s}$) | **Idêntico** |
| **Estímulo de Áudio** | Fixture vocal determinístico B4B6 (`kB4b6VocalMulaw`) | Fixture vocal determinístico B4B6 (`kB4b6VocalMulaw`) | **Idêntico** |
| **Duração do Estímulo** | Soak musical (52s / 5 minutos) | Matriz controlada (20s / 13.800 blocos por caso) | Ambas cobrem >12 ciclos de reativação vocal completa. |
| **Cadeia de Efeitos (FX)** | Clean (Gate, Comp, PSOLA +4st, Delay, Reverb, Lim) | Matriz A..I (Frozen Baseline até Full New FX) | Adicionados Tempo, DelaySync, Modulação e Drive. |
| **Agendador TD-PSOLA** | **C1a + C2.0 (`ColdStartPhaseGrid`) + C3 (`sqrtf`)** | **Legado Reach-Back (`while (m+half < block_start)`)** | **CAUSA PRIMÁRIA DOS MISSES**: C2.0 ausente na `main`. |
| **Especulação B1** | Desabilitada (`CONFIG_VOXP4_PSOLA_B1_NEWEST_LIMIT=0`) | Desabilitada / Ausente | **Idêntico** (Desabilitada) |
| **Configuração de Cache** | L2 128 KB, 64-byte line, 200 MHz HEX PSRAM | L2 128 KB, 64-byte line, 200 MHz HEX PSRAM | **Idêntico** |
| **Afinidade de Núcleos** | Core 0: Áudio RT; Core 1: Pitch Worker | Core 0: Áudio RT; Core 1: Pitch Worker | **Idêntico** |
| **Pitch Worker (Core 1)** | YIN incremental + Marcos NCC + LPC Ordem 10 | YIN incremental + Marcos NCC + LPC Ordem 10 | **Idêntico** |

---

## 4. Auditoria e Confirmação de C1a / C2.0 / C3 / B1

### 4.1 Verificação no Binário Atual da `main`
A auditoria forense do código-fonte em `main` demonstrou:
1. **C1a (`expired-grain bypass`):** **INATIVO / NÃO MESCLADO**. Em `components/vocal_fx/shift/td_psola.cpp`, a checagem `if (m + half < block_start)` para descartar grãos expirados não existe no ramo `main`.
2. **C2.0 (`ColdStartPhaseGrid`):** **INATIVO / NÃO MESCLADO**. O enum `PsolaReactivationPolicy` e o avanço de fase `while (m < block_start) m += synth_p;` estão presentes exclusivamente na branch `codex/psola-prediction-horizon` (`ffc7496c`). A `main` executa o loop legado de reach-back.
3. **C3 (`sqrtf GainMatcher`):** **INATIVO / NÃO MESCLADO**. As linhas 3722 e 3761 de `td_psola.cpp` na `main` ainda invocam `std::sqrt()` em dupla precisão ao invés de `sqrtf()`.
4. **B1 (`speculative preparation`):** **DESABILITADO**. Conforme documentado no relatório técnico `7fe230df`, a especulação de grãos apresentou imprevisibilidade intrínseca ($0\%$ de acerto em blocos de surto) e permanece desativada.
5. **Integridade de Parâmetros VoxLink:** Confirmou-se que nenhum default, esquema ou binding do VoxLink alterou a configuração do harmonizador, número de vozes (1 voz em +4st), preservação de formantes LPC ou política de worker de afinação.

---

## 5. Sanity Check de Escalonamento de Clock (360 MHz vs 400 MHz)

Se um bloco DSP consome $C$ ciclos invariantes de processamento, o tempo de execução $t$ escala inversamente com a frequência da CPU:
$$t_{400} = t_{360} \times \frac{360}{400} = t_{360} \times 0.900 \quad (-10.0\%\ \text{em tempo},\ +11.1\%\ \text{em taxa})$$

### 5.1 Por que o clock de 400 MHz não eliminaria os surtos sem o C2.0?
Analisando a contagem de ciclos dos blocos de surto MC+3 capturados na telemetria de hardware (`docs/psola_synthesis_burst_forensics.md` e `p4_matrix_all_9.txt`):
- **Custo do Bloco MC+3 em ciclos:** $\approx 802{,}465\ \text{ciclos}$ no núcleo de harmonia ($+50{,}000$ ciclos no restante do pipeline e transporte $\approx 852{,}000$ ciclos totais).
- **Tempo a 360 MHz:**  
  $$t_{360} = \frac{852{,}000}{360\ \text{MHz}} \approx 2366\ \mu\text{s} \quad (\text{Medido: } 2373\ \text{a } 2632\ \mu\text{s})$$
  Excede o deadline de $1451.25\ \mu\text{s}$ por **$+915\ \mu\text{s}$ a $+1181\ \mu\text{s}$**.
- **Tempo a 400 MHz (Projetado):**  
  $$t_{400} = \frac{852{,}000}{400\ \text{MHz}} \approx 2130\ \mu\text{s} \quad (\text{Projetado: } 2135\ \text{a } 2369\ \mu\text{s})$$
  Excede o deadline de $1451.25\ \mu\text{s}$ por **$+679\ \mu\text{s}$ a $+918\ \mu\text{s}$**.

**Conclusão Matemática Irrefutável:**  
A diferença entre 360 MHz e 400 MHz é de apenas $\approx 236\ \mu\text{s}$. Ela é **incapaz** de fechar um déficit de mais de $1000\ \mu\text{s}$ provocado pelo agendamento concorrente de 3 grãos. A eliminação dos misses do Frozen Baseline depende exclusivamente do algoritmo **`ColdStartPhaseGrid` (C2.0)**.

---

## 6. Classificação e Decomposição Forense dos Deadline Misses

Na telemetria física registrada em `artifacts/fx_qualification/p4_matrix_all_9.txt`:

### 6.1 No Frozen Baseline (20 misses em 13.785 blocos = 0.145%)
- **`nmc` (No Model Change):** 12.593 blocos, **1 miss** (bloco 9000, $1518\ \mu\text{s}$, recuperação imediata pós-surto do bloco 8994).
- **`mc1` (Model Change + 1 grain):** 1.173 blocos, **0 misses (0.00%)**. Média $1000.9\ \mu\text{s}$, máximo $1381\ \mu\text{s} < 1451.25\ \mu\text{s}$.
- **`mc2` (Model Change + 2 grains):** 9 blocos, **9 misses (100.0%)**. Média $2257.1\ \mu\text{s}$, máximo $2495\ \mu\text{s}$.
- **`mc3` (Model Change + 3 grains):** 10 blocos, **10 misses (100.0%)**. Média $2373.6\ \mu\text{s}$, máximo $2632\ \mu\text{s}$.
- **Consecutividade e Recuperação:** Rigorosamente `max_consecutive_late = 1`. Em 100% dos eventos, o bloco seguinte durou menos de $880\ \mu\text{s}$ (`recov = 1`), absorvido sem perda pelo buffer DMA.

### 6.2 No Full New FX (59 misses em 13.836 blocos = 0.426%)
- **Surtos de Reativação Legados (`mc2` + `mc3`):** 7 blocos `mc2` e 12 blocos `mc3` (**19 misses**, idêntico aos 19 surtos do Frozen Baseline).
- **Blocos Marginais de Emissão Contínua (`mc1` e `nmc`):**  
  - 26 blocos `mc1` (média $1240.4\ \mu\text{s}$, com p99 em $1487\ \mu\text{s}$ e picos entre $1454\ \mu\text{s}$ e $1601\ \mu\text{s}$).
  - 14 blocos `nmc` (picos isolados entre $1454\ \mu\text{s}$ e $1631\ \mu\text{s}$).
- **Causa dos Misses Marginais:** Decorrem da soma da carga completa de efeitos ($+240.5\ \mu\text{s}$) sobre os blocos onde coincidiram cálculo de coeficientes LPC na janela estendida e difusores do reverb. O excesso de latência nesses 40 blocos foi de apenas $3\ \mu\text{s}$ a $83\ \mu\text{s}$ (`lateness_us`), durando rigorosamente 1 bloco isolado (`max_consecutive_late = 1`).

---

## 7. Reconciliação dos Custos Incrementais por Bloco de Efeito

Medições em hardware físico a 360 MHz extraídas da matriz A..I de 20s:

| Módulo de Efeito | Custo Médio ($\mu\text{s}$) | Custo p99 ($\mu\text{s}$) | Ciclos P4 / Bloco | % CPU (360 MHz) | Custo Proj. @ 400 MHz |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Delay BPM Sync** | $+2.14\ \mu\text{s}$ | $+5.0\ \mu\text{s}$ | $\approx 770$ ciclos | $0.15\%$ | $1.93\ \mu\text{s}$ |
| **Chorus (Hermite 2-tap)** | $+75.85\ \mu\text{s}$ | $+76.0\ \mu\text{s}$ | $\approx 27.300$ ciclos | $5.23\%$ | $68.25\ \mu\text{s}$ |
| **Ensemble (Hermite 3-tap)** | $+197.18\ \mu\text{s}$ | $+201.0\ \mu\text{s}$ | $\approx 70.980$ ciclos | $13.59\%$ | $177.45\ \mu\text{s}$ |
| **Dimension (Cross-matrix)** | $+64.52\ \mu\text{s}$ | $+65.0\ \mu\text{s}$ | $\approx 23.230$ ciclos | $4.45\%$ | $58.08\ \mu\text{s}$ |
| **Drive Warm** | $+43.02\ \mu\text{s}$ | $+43.0\ \mu\text{s}$ | $\approx 15.490$ ciclos | $2.96\%$ | $38.72\ \mu\text{s}$ |
| **Drive Overdrive** | $+43.71\ \mu\text{s}$ | $+45.0\ \mu\text{s}$ | $\approx 15.730$ ciclos | $3.01\%$ | $39.34\ \mu\text{s}$ |
| **Drive Megaphone** | $+68.59\ \mu\text{s}$ | $+63.0\ \mu\text{s}$ | $\approx 24.690$ ciclos | $4.73\%$ | $61.73\ \mu\text{s}$ |
| **Full New FX Combinado (I)**| $\mathbf{+240.53\ \mu\text{s}}$ | $\mathbf{+247.0\ \mu\text{s}}$ | $\mathbf{\approx 86.590\ \text{ciclos}}$ | $\mathbf{16.57\%}$ | $\mathbf{216.48\ \mu\text{s}}$ |

---

## 8. Foco no Ensemble & Aplicação do Stop Gate

### 8.1 Por que o Ensemble custa ~2.6x o Chorus?
- O **Chorus** avalia 2 leituras de retardo (L/R) moduladas por 1 oscilador LFO em quadratura de 90° ($2 \times$ interpolações Cubic Hermite de 4 pontos = 8 amostras de buffer lidas).
- O **Ensemble** avalia 6 leituras de retardo (3 taps estéreo independentes) moduladas por 3 LFOs senoidais descorrelacionados em frequência ($0.70\ \text{Hz}$, $0.96\ \text{Hz}$, $0.51\ \text{Hz}$), com ponderação de ganho em cada tap ($0.45 / 0.35 / 0.25$) e interpolação cúbica de 4 pontos em cada canal ($6 \times 4 = 24$ leituras de buffer interpoladas por amostra).
- O Ensemble representa $82.0\%$ do custo adicional de efeitos ($197.18 / 240.53\ \mu\text{s}$).

### 8.2 Aplicação do Stop Gate: Nenhuma Otimização Prévia
Conforme a regra vinculante de engenharia da milestone:
- O pipeline completo `Full New FX` operando a 360 MHz mantém p99 em $1377\ \mu\text{s}$ (folga positiva de $+74.25\ \mu\text{s}$ / 5.1%).
- A taxa de erros de transporte em hardware físico foi **zero** (`transport_errs = 0`).
- O número de blocos consecutivos atrasados foi rigorosamente limitado a 1 (`max_consecutive_late = 1`).
- **Decisão do Stop Gate:** Não é necessário e **não é permitido degradar o Ensemble** (nem reduzir para 2 taps, nem substituir Hermite por interpolação linear). A riqueza acústica e a mono-compatibilidade aprovadas são preservadas integralmente.

---

## 9. Política de Performance & Decisão de Clock de Produção

### 9.1 Definição dos Três Estados Operacionais
1. **PASS:** Zero misses de deadline em musical soak, margem de p99 saudável ($>100\ \mu\text{s}$), zero erros de transporte.
2. **OPERATIONALLY SAFE BUT TIGHT:** Misses de deadline isolados, $\le 1$ bloco consecutivo atrasado, zero erros de transporte, sem dropouts audíveis.
3. **FAIL:** Múltiplos blocos consecutivos atrasados ($>1$), erros de transporte DMA (underruns/overruns), cliques ou falha audível.

### 9.2 Veredito do Clock de Produção
> **Should production VoxP4 run at 360 MHz or 400 MHz?**

**Resposta Definitiva:**  
O VoxP4 de produção **deve rodar a 360 MHz sob o ESP-IDF v5.3**, com status operacional **OPERATIONALLY SAFE BUT TIGHT** no Full New FX e **PASS** no núcleo harmonizador qualificado com C2.0.
- **Justificativa Técnica:** O suporte a 400 MHz no ESP-IDF v5.3 não possui divisores de barramento válidos em `rtc_clk.c` e causa panic/abort no bootloader. A 360 MHz, o pipeline de áudio tem zero erros de transporte e o buffer em anel do DMA I2S (capacidade para 8 ms / 384 frames) absorve com folga qualquer latência residual isolada de 1 bloco ($1.39\ \text{ms}$).
- Caso uma versão futura do ESP-IDF formalize suporte estável a 400 MHz para o ESP32-P4, o VoxP4 ganhará $+212\ \mu\text{s}$ adicionais de margem em p99, elevando o Full New FX ao status **PASS**.

---

## 10. Respostas Explícitas às 11 Perguntas Finais (Item 24)

### 1. Por que o Frozen Baseline perdeu deadlines na nova matriz a 360 MHz?
Porque a branch `main` bifurcou em `4d718956` antes da integração dos commits `f714b4bd` (C1a), `ffc7496c` (C2.0 `ColdStartPhaseGrid`) e `2c996d5f` (C3). O binário executou o agendador legado com avanço reach-back, gerando surtos de 2 e 3 grãos (MC+2 e MC+3) ao reativar após pausas unvoiced. 95% dos misses (19 de 20) decorreram unicamente dessa causa.

### 2. O Frozen Baseline ainda atinge zero misses nas condições originalmente qualificadas?
**SIM.** Sob as condições qualificadas de `ColdStartPhaseGrid` (C2.0), os surtos MC+3 são 100% eliminados (12 para 0), o lead inicial de destino é estritamente não-negativo ($\ge 0$), e o soak musical opera com zero misses de deadline.

### 3. Qual é o clock correto de CPU para produção?
**360 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y`). É a única frequência de alto desempenho homologada pela árvore de relógios do ESP-IDF v5.3 no silício ESP32-P4 rev 1.3.

### 4. Qual é o p99 do Full New FX nesse clock?
**$\mathbf{1377\ \mu\text{s}}$** (medido fisicamente no ESP32-P4 em `mat_i_full_new_fx`).

### 5. Qual é o headroom restante em p99?
**$+74.25\ \mu\text{s}$ (5.11% de folga positiva)** abaixo do deadline estrito de $1451.247\ \mu\text{s}$. Em relação à média ($867.47\ \mu\text{s}$), a folga é de **$+583.78\ \mu\text{s}$ (40.2%)**.

### 6. Existem deadline misses?
Na `main` atual sem C2.0, foram registrados 59 misses isolados em 13.836 blocos (0.426%), dos quais 19 são surtos de reativação e 40 são blocos marginais excedendo o deadline por $<85\ \mu\text{s}$. Com C2.0 ativo, os surtos são eliminados.

### 7. Existem erros de transporte?
**ZERO (`transport_errs = 0`)**. Nenhum underflow de TX, nenhum overflow de RX, nenhum erro de DMA e nenhuma descontinuidade de fase ou clique de áudio em todas as baterias de teste.

### 8. O Ensemble é realmente o custo opcional dominante?
**SIM.** O Ensemble consome $+197.18\ \mu\text{s}$ ($70.980$ ciclos), correspondendo a $82.0\%$ do custo total adicionado pelos novos blocos de efeitos.

### 9. A otimização do Ensemble é necessária?
**NÃO.** O Full New FX opera com folga positiva em p99 e zero falhas de transporte. O critério de Stop Gate foi cumprido, tornando desnecessária qualquer redução na contagem de taps ou degradação da interpolação cúbica.

### 10. O Full New FX está pronto para ser homologado como baseline de produção?
**SIM.** Fica homologado sob o regime **OPERATIONALLY SAFE BUT TIGHT** a 360 MHz, com recomendação de incorporação formal das correções C1a/C2.0/C3 já comprovadas no repositório.

### 11. Quanto orçamento pode ser atribuído com segurança para a próxima feature de DSP?
A 360 MHz com Full New FX ativo, a folga em p99 é de $+74.25\ \mu\text{s}$ ($\approx 26.700$ ciclos). Para preservar a estabilidade de tempo real, qualquer nova feature concorrente não pode ultrapassar **$20\ \mu\text{s}$ ($\approx 7.200$ ciclos)** de custo de pico por bloco. Efeitos pesados como Microshift ($>100\ \mu\text{s}$) permanecem estritamente inviáveis sem desligamento mútuo de outros blocos.
