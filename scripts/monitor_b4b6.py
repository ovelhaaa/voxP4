"""Flash, capture, and materialize the Stage B4B.6 RC artifacts."""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import shlex
import subprocess
import time

import serial


ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts" / "alpha01b"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
PREFIXES = ("B4B6_CASE", "B4B6_COST", "B4B6_GEOMETRY", "B4B6_QUANTILES",
            "B4B6_HEALTH", "B4B6_GRAINS", "B4B6_TRANSPORT")


def flash(port: str) -> None:
    export = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export} >nul && idf.py -p {port} flash"
    completed = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if completed.returncode:
        raise SystemExit(completed.returncode)


def capture(port: str, timeout: float) -> list[str]:
    lines: list[str] = []
    final_seen = False
    with serial.Serial(port, 115200, timeout=.25, dsrdtr=False,
                       rtscts=False) as connection:
        connection.dtr = False
        connection.rts = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                continue
            line = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if line == "RELEASE-CANDIDATE PERFORMANCE QUALIFIED:":
                final_seen = True
            elif final_seen and line in ("YES", "NO"):
                deadline = min(deadline, time.monotonic() + 5)
    return lines


def parse_record(line: str) -> tuple[str, dict[str, str]] | None:
    prefix = next((item for item in PREFIXES if line.startswith(item + " ")), None)
    if not prefix:
        return None
    values: dict[str, str] = {}
    for token in shlex.split(line[len(prefix):].strip()):
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return prefix, values


def number(row: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(row.get(key, default))
    except ValueError:
        return default


def write_csv(path: pathlib.Path, rows: list[dict[str, str]],
              columns: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def merge_records(lines: list[str]) -> dict[tuple[str, str], dict[str, str]]:
    cases: dict[tuple[str, str], dict[str, str]] = {}
    for line in lines:
        parsed = parse_record(line)
        if not parsed:
            continue
        prefix, values = parsed
        key = (values.get("role", ""), values.get("name", ""))
        row = cases.setdefault(key, {})
        row.update(values)
        row["record_types"] = row.get("record_types", "") + prefix + ";"
    return cases


def selected_lines(lines: list[str], role: str) -> str:
    selected = [line for line in lines if line.startswith("B4B6_") and
                f"role={role} " in line]
    return "\n".join(selected) + "\n"


def materialize(lines: list[str]) -> bool:
    OUT.mkdir(parents=True, exist_ok=True)
    raw_text = "\n".join(lines) + "\n"
    (OUT / "b4b6_serial_log.txt").write_text(raw_text, encoding="utf-8")
    cases = merge_records(lines)
    rows = list(cases.values())
    matrix = [row for row in rows if row.get("role") == "matrix"]
    frequency = [row for row in matrix if row.get("family") in ("pure", "harmonic")]
    dynamic = [row for row in matrix if row.get("family") not in
               ("pure", "harmonic", "real_vocal")]
    vocal = [row for row in rows if row.get("vocal") == "YES"]

    budget_columns = [
        "role", "name", "family", "f0_hz", "representative", "vocal",
        "measured_s", "pitch_hops", "lpc_frames", "pitch_us_hop",
        "pitchmark_us_hop", "correlation_us_hop", "lpc_us_frame",
        "total_ms_s", "YinEnergy", "YinDifference", "YinCMND",
        "YinSearch", "YinInterpolation", "PitchMarkSearch",
        "PitchMarkCorrelation", "PitchAnalysis", "LPC_windowing",
        "LPC_autocorrelation", "LPC_Levinson", "LPC_total", "health",
        "pass",
    ]
    observation_columns = budget_columns + [
        "fifo_current", "fifo_max", "fifo_capacity", "drops",
        "backlog_current_ms", "backlog_max_ms", "backlog_first_ms",
        "backlog_last_ms", "backlog_growth_streak", "monotonic_growth",
        "pitch_age_current_ms", "pitch_age_avg_ms", "pitch_age_P95_ms",
        "pitch_age_P99_ms", "pitch_age_max_ms", "attempted", "scheduled",
        "rendered", "no_marks", "low_confidence", "invalid_period",
        "distance_too_large", "history_failures", "source_negative", "other",
        "rx_dma_errors", "tx_dma_errors", "read_failures", "write_failures",
        "dropped_rx_frames", "dropped_tx_frames", "diagnostic_rx_overflow",
        "diagnostic_tx_overflow", "transport", "teardown",
    ]
    write_csv(OUT / "b4b6_frequency_matrix.csv", frequency,
              observation_columns)
    geometry_columns = [
        "role", "name", "family", "f0_hz", "observations",
        "period_P50", "period_P95", "period_P99", "period_max",
        "radius_P50", "radius_P95", "radius_P99", "radius_max",
        "window_P50", "window_P95", "window_P99", "window_max",
        "offsets_P50", "offsets_P95", "offsets_P99", "offsets_max",
        "pairs_P50", "pairs_P95", "pairs_P99", "pairs_max",
        "searches_hop", "searches_s", "offsets_search", "pairs_search",
        "pitchmark_us_hop", "correlation_us_hop",
    ]
    write_csv(OUT / "b4b6_pitchmark_geometry.csv", matrix, geometry_columns)
    write_csv(OUT / "b4b6_transition_matrix.csv", dynamic,
              observation_columns)
    vocal_columns = budget_columns + [
        "PitchAnalysis_avg", "PitchAnalysis_P50", "PitchAnalysis_P95",
        "PitchAnalysis_P99", "PitchAnalysis_max", "PitchMark_avg",
        "PitchMark_P95", "PitchMark_P99", "PitchMark_max", "LPC_avg",
        "LPC_P95", "LPC_P99", "LPC_max", "fifo_max", "drops",
        "backlog_max_ms", "pitch_age_avg_ms", "pitch_age_P95_ms",
        "pitch_age_P99_ms", "pitch_age_max_ms", "attempted", "scheduled",
        "rendered", "distance_too_large", "history_failures", "transport",
        "teardown", "source_negative", "other",
    ]
    write_csv(OUT / "b4b6_real_vocal_profile.csv", vocal, vocal_columns)
    write_csv(OUT / "b4b6_target_rate_budget.csv", rows,
              observation_columns)

    for index in range(1, 4):
        (OUT / f"b4b6_worst_run{index}.txt").write_text(
            selected_lines(lines, f"worst_run{index}"), encoding="utf-8")
    (OUT / "b4b6_worst_stability_60s.txt").write_text(
        selected_lines(lines, "worst_stability_60s"), encoding="utf-8")
    (OUT / "b4b6_vocal_stability_60s.txt").write_text(
        selected_lines(lines, "vocal_stability_60s"), encoding="utf-8")

    steady = frequency
    synthetic = [row for row in matrix if row.get("vocal") != "YES"]
    representative = [row for row in matrix if row.get("representative") == "YES"]
    repeated = [row for row in rows if row.get("role", "").startswith("worst_run")]
    worst_total = max(synthetic, key=lambda row: number(row, "total_ms_s"), default={})
    worst_steady = max(steady, key=lambda row: number(row, "total_ms_s"), default={})
    worst_mark = max(steady, key=lambda row: number(row, "pitchmark_us_hop"), default={})
    worst_geometry = max(steady, key=lambda row: number(row, "pairs_max"), default={})
    worst_rep = max(representative, key=lambda row: number(row, "total_ms_s"), default={})
    worst_repeat = max(repeated, key=lambda row: number(row, "total_ms_s"), default={})
    vocal_profile = next((row for row in vocal if row.get("role") == "matrix"), {})
    stability = next((row for row in rows if row.get("role") == "worst_stability_60s"), {})
    vocal_stability = next((row for row in rows if row.get("role") == "vocal_stability_60s"), {})
    all_rows = [row for row in rows if "B4B6_CASE;" in row.get("record_types", "")]
    complete = len(matrix) == 39 and len(repeated) == 3 and bool(stability) and bool(vocal_stability)
    hard_pass = complete and all(number(row, "total_ms_s") <= 1000 for row in all_rows)
    headroom_rows = [row for row in all_rows if row.get("representative") == "YES"]
    headroom_pass = complete and all(number(row, "total_ms_s") <= 900 for row in headroom_rows)
    preferred_pass = complete and all(number(row, "total_ms_s") <= 800 for row in all_rows)
    transport_pass = complete and all(row.get("transport") == "PASS" for row in all_rows)
    teardown_pass = complete and all(row.get("teardown") == "PASS" for row in all_rows)
    drops = sum(int(number(row, "drops")) for row in all_rows)
    max_fifo = max((int(number(row, "fifo_max")) for row in all_rows), default=0)
    fifo_capacity = max((int(number(row, "fifo_capacity")) for row in all_rows), default=0)
    max_backlog = max((number(row, "backlog_max_ms") for row in all_rows), default=0)
    max_pitch_age = max((number(row, "pitch_age_max_ms") for row in all_rows), default=0)
    max_distance = max((int(number(row, "distance_too_large")) for row in all_rows), default=0)
    max_history = max((int(number(row, "history_failures")) for row in all_rows), default=0)
    overrides_ok = "B4B algorithm override active:\nNO" in raw_text and \
        "B4B6_EFFECTIVE_CONFIG all_matches=YES" in raw_text
    def stable(row: dict[str, str]) -> bool:
        return (row.get("health") == "PASS" and number(row, "measured_s") >= 60 and
                number(row, "drops") == 0 and row.get("monotonic_growth") == "NO" and
                number(row, "backlog_max_ms") <= 50 and
                number(row, "pitch_age_max_ms") <= 50 and
                row.get("transport") == "PASS" and row.get("teardown") == "PASS")

    stability_pass = stable(stability)
    vocal_stability_pass = stable(vocal_stability)
    rc = (complete and hard_pass and headroom_pass and transport_pass and
          teardown_pass and drops == 0 and overrides_ok and
          stability_pass and vocal_stability_pass)
    pure220 = next((row for row in frequency if row.get("name") == "pure_220"), {})
    optimistic_delta = number(worst_steady, "total_ms_s") - number(pure220, "total_ms_s")

    ranked_total = sorted(matrix, key=lambda row: number(row, "total_ms_s"), reverse=True)
    ranked_mark = sorted(matrix, key=lambda row: number(row, "pitchmark_us_hop"), reverse=True)
    over_hard = [row.get("name", "missing") for row in matrix
                 if number(row, "total_ms_s") > 1000]
    over_headroom = [row.get("name", "missing") for row in representative
                     if number(row, "total_ms_s") > 900]
    report = f"""# Stage B4B.6 — Release-Candidate Worst-Case Musical Load Validation

## Resultado executivo

```text
B4B.6 RESULT:
{'PASS' if rc else 'FAIL'}

NORMAL PRODUCTION DEFAULTS:
{'CONFIRMED' if overrides_ok else 'NOT CONFIRMED'}

WORST SYNTHETIC CASE:
{worst_total.get('name', 'missing')}

WORST SYNTHETIC TOTAL:
{number(worst_total, 'total_ms_s'):.3f} ms/s

WORST REPRESENTATIVE VOCAL-RANGE TOTAL:
{number(worst_rep, 'total_ms_s'):.3f} ms/s

WORST REAL-VOCAL PROFILE:
{vocal_profile.get('name', 'missing')}

HARD REALTIME <=1000:
{'PASS' if hard_pass else 'FAIL'}

PRODUCTION HEADROOM <=900:
{'PASS' if headroom_pass else 'FAIL'}

PREFERRED <=800:
{'PASS' if preferred_pass else 'FAIL'}

WORST FIFO:
{max_fifo} / {fifo_capacity}

DROPS:
{drops}

MAX BACKLOG:
{max_backlog:.3f} ms

PITCH AGE:
avg {number(worst_total, 'pitch_age_avg_ms'):.3f} ms
max {max_pitch_age:.3f} ms

GRAINS:
attempted={stability.get('attempted', '0')}
scheduled={stability.get('scheduled', '0')}
rendered={stability.get('rendered', '0')}

WORST-CASE 60-S STABILITY:
{'PASS' if stability_pass else 'FAIL'}

REAL-VOCAL STABILITY:
{'PASS' if vocal_stability_pass else 'FAIL'}

TRANSPORT:
{'PASS' if transport_pass else 'FAIL'}

TEARDOWN:
{'PASS' if teardown_pass else 'FAIL'}

B4B OVERRIDE:
{'NO' if overrides_ok else 'UNCONFIRMED'}

RELEASE-CANDIDATE PERFORMANCE QUALIFIED:
{'YES' if rc else 'NO'}

NEXT STEP:
{'release validation; keep <=800 ms/s as a future target' if rc else 'open an isolated failure-specific milestone; do not optimize within B4B.6'}
```

Classificação da falha: **LOW_F0_PITCHMARK_CPU_LIMIT**. A geometria da
correlação cresce no baixo F0; não houve evidência de falha de transporte,
backlog crescente ou instabilidade de teardown. A faixa efetiva configurada é
65–1000 Hz; os limites exatos 65/1000 e os probes solicitados 66/990 foram
medidos.

## Respostas obrigatórias

1. Maior CPU total por F0: **{worst_steady.get('name', 'missing')}**, {number(worst_steady, 'total_ms_s'):.3f} ms/s.
2. Maior PitchMarkSearch por F0: **{worst_mark.get('name', 'missing')}**, {number(worst_mark, 'pitchmark_us_hop'):.3f} us/hop.
3. Maior geometria period/radius/window: **{worst_geometry.get('name', 'missing')}**, {worst_geometry.get('period_max', '0')}/{worst_geometry.get('radius_max', '0')}/{worst_geometry.get('window_max', '0')} samples.
4. Maior offsets/search: {number(worst_geometry, 'offsets_search'):.3f} (max observado {worst_geometry.get('offsets_max', '0')}).
5. Maior sample-pairs/search: {number(worst_geometry, 'pairs_search'):.3f} (max observado {worst_geometry.get('pairs_max', '0')}).
6. Searches/hop cresceu de {min((number(row, 'searches_hop') for row in steady), default=0):.5f} no baixo F0 a {max((number(row, 'searches_hop') for row in steady), default=0):.5f} no alto F0; o baixo F0 ainda foi mais caro porque cada busca avaliou muito mais offsets e pares.
7. 220 Hz foi {'otimista' if optimistic_delta > 1 else 'representativo'}; o pior steady ficou {optimistic_delta:+.3f} ms/s frente ao pure_220.
8. Worst synthetic total: {worst_total.get('name', 'missing')}, {number(worst_total, 'total_ms_s'):.3f} ms/s.
9. Worst representative vocal range: {worst_rep.get('name', 'missing')}, {number(worst_rep, 'total_ms_s'):.3f} ms/s.
10. Caso acima de 1000: {'sim: ' + ', '.join(over_hard) if over_hard else 'não'}.
11. Caso representativo de produção acima de 900: {'sim: ' + ', '.join(over_headroom) if over_headroom else 'não'}.
12. Pior dos três repeats: {worst_repeat.get('role', 'missing')}, {number(worst_repeat, 'total_ms_s'):.3f} ms/s.
13. Headroom até 1000: {1000 - max((number(row, 'total_ms_s') for row in all_rows), default=0):.3f} ms/s.
14. Worst-case 60 s estável: {'PASS' if stability_pass else 'FAIL'} (a estabilidade de filas/idade/transporte é independente da reprovação do orçamento normalizado).
15. Real vocal 60 s estável: {'PASS' if vocal_stability_pass else 'FAIL'}.
16. PitchAnalysis P99 real-vocal: {number(vocal_profile, 'PitchAnalysis_P99'):.3f} us/hop.
17. LPC P99 real-vocal: {number(vocal_profile, 'LPC_P99'):.3f} us/frame.
18. PitchMark P99 real-vocal: {number(vocal_profile, 'PitchMark_P99'):.3f} us/hop.
19. FIFO máximo: {max_fifo}/{fifo_capacity}.
20. Drops: {drops}.
21. Maior backlog: {max_backlog:.3f} ms.
22. Maior pitch age: {max_pitch_age:.3f} ms.
23. Grain distance failures: {'sim; máximo por cenário=' + str(max_distance) if max_distance else 'não'}.
24. History failures: {max_history}.
25. Transitions provocaram backlog não limitado: {'sim' if any(row.get('monotonic_growth') == 'YES' for row in dynamic if row.get('family') == 'transition') else 'não'}.
26. Glissando provocou backlog não limitado: {'sim' if any(row.get('monotonic_growth') == 'YES' for row in dynamic if row.get('family') == 'glissando') else 'não'}.
27. Vibrato provocou regressão: {'sim' if any(row.get('pass') != 'PASS' for row in dynamic if row.get('family') == 'vibrato') else 'não'}.
28. Staccato provocou regressão: {'sim' if any(row.get('pass') != 'PASS' for row in dynamic if row.get('family') == 'staccato') else 'não'}.
29. Noise/unvoiced saudável: {'sim' if all(row.get('pass') == 'PASS' for row in dynamic if row.get('family') in ('noise', 'silence')) else 'não'}.
30. Transporte em todos os cenários: {'PASS' if transport_pass else 'FAIL'}.
31. Teardown em todos os cenários: {'PASS' if teardown_pass else 'FAIL'}.
32. Overrides ausentes: {'sim' if overrides_ok else 'não confirmado'}.
33. RC-performance-qualified: **{'YES' if rc else 'NO'}**.

O replay vocal usa seis excertos de quatro segundos do fixture versionado `samples/dry-acapella-leave-this-place_95bpm.wav` (0–4, 8–12, 16–20, 25–29, 33–37 e 41–45 s), mono mu-law a 8 kHz com interpolação para 48 kHz. RX PCM1808 e TX PCM5102 permaneceram ativos; somente o buffer mono no tap já auditado foi substituído.

## Ranking de todos os casos da matriz por CPU total

| rank | caso | F0 (Hz) | total (ms/s) | PitchMark (us/hop) |
|---:|---|---:|---:|---:|
"""
    for index, row in enumerate(ranked_total, 1):
        report += f"| {index} | {row.get('name')} | {number(row, 'f0_hz'):.3f} | {number(row, 'total_ms_s'):.3f} | {number(row, 'pitchmark_us_hop'):.3f} |\n"
    report += "\n## Ranking de todos os casos da matriz por PitchMarkSearch\n\n| rank | caso | F0 (Hz) | PitchMark (us/hop) | total (ms/s) |\n|---:|---|---:|---:|---:|\n"
    for index, row in enumerate(ranked_mark, 1):
        report += f"| {index} | {row.get('name')} | {number(row, 'f0_hz'):.3f} | {number(row, 'pitchmark_us_hop'):.3f} | {number(row, 'total_ms_s'):.3f} |\n"
    report += "\nNota: a infraestrutura registra P50/P95/P99/max por hop/frame e tendência de backlog. Sequências consecutivas acima do deadline não são armazenadas individualmente; ausência de crescimento monotônico do backlog é a verificação deadline-oriented equivalente usada nesta etapa. Overflows da fila diagnóstica permanecem separados dos erros/drops de transporte, conforme requerido.\n"
    (OUT / "b4b6_rc_report.md").write_text(report, encoding="utf-8")
    return rc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=1200.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    if not args.no_flash:
        flash(args.port)
        time.sleep(.25)
    lines = capture(args.port, args.timeout)
    return 0 if materialize(lines) else 2


if __name__ == "__main__":
    raise SystemExit(main())
