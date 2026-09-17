"""Stage B4C.3B Exact Source-Grain Reuse & Grain-Burst Flattening Audit Runner.

Flashes firmware to ESP32-P4 on COM11, captures serial telemetry,
and generates all required CSVs, raw logs, and comprehensive markdown report in artifacts/alpha01c/.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts" / "alpha01c"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def flash(port: str) -> None:
    print(f"Flashing firmware to {port}...", flush=True)
    export = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export} >nul && idf.py -p {port} flash"
    completed = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if completed.returncode != 0:
        print(f"Flashing failed with return code {completed.returncode}", flush=True)
        sys.exit(completed.returncode)
    print("Flashing successful!\n", flush=True)


def capture(port: str, timeout: float = 950.0, baud: int = 115200) -> list[str]:
    lines: list[str] = []
    final_seen = False
    print(f"Opening serial port {port} at {baud} baud (timeout={timeout}s)...", flush=True)
    time.sleep(1.5)
    connection = None
    for attempt in range(12):
        try:
            connection = serial.Serial(port, baud, timeout=0.25, dsrdtr=False, rtscts=False)
            break
        except Exception as e:
            print(f"Waiting for {port} to become ready ({e})...", flush=True)
            time.sleep(1.0)
    if connection is None:
        print(f"Failed to open {port} after multiple attempts.", flush=True)
        sys.exit(1)

    with connection:
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
            if "STAGE B4C.3B COMPLETE" in line or "HARMONIZER REALTIME QUALIFIED:" in line:
                final_seen = True
            elif final_seen and "================================================================================" in line:
                deadline = min(deadline, time.monotonic() + 3.0)
    return lines


def parse_key_value_line(prefix: str, line: str) -> dict[str, str]:
    content = line[len(prefix):].strip()
    result = {}
    for token in shlex.split(content):
        if "=" in token:
            k, v = token.split("=", 1)
            result[k] = v
    return result


def write_csv(path: pathlib.Path, rows: list[dict[str, any]], columns: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Generated CSV: {path}")


def process_serial_log(lines: list[str]) -> None:
    OUT.mkdir(parents=True, exist_ok=True)

    # 1. Raw serial file
    raw_serial_file = OUT / "b4c3b_raw_serial.txt"
    raw_serial_file.write_text("\n".join(lines), encoding="utf-8")
    print(f"Saved: {raw_serial_file}")

    capacity_rows = []
    candidates_rows = []
    full_matrix_rows = []
    top100_rows = []
    regression_rows = []
    stability_rows = []
    buffer_rows = []

    cases_dict = {}
    metrics_dict = {}
    psola_timings_dict = {}
    residual_stats_dict = {}
    source_grain_dict = {}
    slack_dict = {}

    in_buffer_audit = False

    for line in lines:
        if line.startswith("B4C3B_CACHE_CAPACITY "):
            vals = parse_key_value_line("B4C3B_CACHE_CAPACITY", line)
            capacity_rows.append(vals)

        elif line.startswith("B4C3B_CANDIDATE_SUMMARY "):
            vals = parse_key_value_line("B4C3B_CANDIDATE_SUMMARY", line)
            candidates_rows.append(vals)

        elif line.startswith("B4C3B_CASE "):
            vals = parse_key_value_line("B4C3B_CASE", line)
            cases_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3B_BLOCK_METRICS "):
            vals = parse_key_value_line("B4C3B_BLOCK_METRICS", line)
            metrics_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3B_PSOLA_KERNEL_TIMINGS "):
            vals = parse_key_value_line("B4C3B_PSOLA_KERNEL_TIMINGS", line)
            psola_timings_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3B_RESIDUAL_CACHE_STATS "):
            vals = parse_key_value_line("B4C3B_RESIDUAL_CACHE_STATS", line)
            residual_stats_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3B_SOURCE_GRAIN_AUDIT "):
            vals = parse_key_value_line("B4C3B_SOURCE_GRAIN_AUDIT", line)
            source_grain_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3B_SCHEDULING_SLACK "):
            vals = parse_key_value_line("B4C3B_SCHEDULING_SLACK", line)
            slack_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3B_TOP100_ROW "):
            tokens = line[len("B4C3B_TOP100_ROW "):].strip().split(",")
            if len(tokens) >= 23:
                top100_rows.append({
                    "rank": tokens[0], "block_index": tokens[1], "runtime_us": tokens[2],
                    "base_us": tokens[3], "v0_us": tokens[4], "v1_us": tokens[5],
                    "warp_us": tokens[6], "sched_v0": tokens[7], "sched_v1": tokens[8],
                    "rend_v0": tokens[9], "rend_v1": tokens[10], "samples": tokens[11],
                    "fir_samples": tokens[12], "fir_reused": tokens[13],
                    "hits_v0": tokens[14], "hits_v1": tokens[15],
                    "miss_v0": tokens[16], "miss_v1": tokens[17],
                    "ov_v0": tokens[18], "ov_v1": tokens[19],
                    "slack_blocks": tokens[20], "recovery": tokens[21], "pitch_changed": tokens[22]
                })

        elif line.startswith("B4C3B_BURST_REGRESSION "):
            vals = parse_key_value_line("B4C3B_BURST_REGRESSION", line)
            regression_rows.append(vals)

        elif line.startswith("B4C3B_BUFFER_AUDIT_START"):
            in_buffer_audit = True
        elif line.startswith("B4C3B_BUFFER_AUDIT_END"):
            in_buffer_audit = False
        elif in_buffer_audit and line.startswith("B4C3B_BUFFER "):
            vals = parse_key_value_line("B4C3B_BUFFER", line)
            buffer_rows.append(vals)

    # Build full matrix CSV
    matrix_case_order = [
        "dry_path_baseline", "harmonizer_1v_vocal", "harmonizer_2v_vocal",
        "f0_65_extreme_bass", "f0_80_bass", "f0_110_a2", "f0_147_d3",
        "f0_220_a3", "f0_440_a4", "pitch_step_burst",
        "stability_harmonizer_60s", "stability_worst_synth_60s"
    ]

    for cname in matrix_case_order:
        c_val = cases_dict.get(cname, {})
        m_val = metrics_dict.get(cname, {})
        k_val = psola_timings_dict.get(cname, {})
        r_val = residual_stats_dict.get(cname, {})
        s_val = source_grain_dict.get(cname, {})
        sl_val = slack_dict.get(cname, {})

        if not m_val:
            continue

        full_matrix_rows.append({
            "case_name": cname,
            "suite": c_val.get("suite", ""),
            "nominal_f0": c_val.get("f0", ""),
            "blocks": c_val.get("blocks", ""),
            "wall_s": c_val.get("wall_s", ""),
            "avg_us": m_val.get("avg_us", ""),
            "p50_us": m_val.get("p50_us", ""),
            "p90_us": m_val.get("p90_us", ""),
            "p95_us": m_val.get("p95_us", ""),
            "p99_us": m_val.get("p99_us", ""),
            "p999_us": m_val.get("p999_us", ""),
            "max_us": m_val.get("max_us", ""),
            "cpu_pct": m_val.get("cpu_pct", ""),
            "margin_us": m_val.get("margin_us", ""),
            "misses": m_val.get("misses", ""),
            "late_pct": m_val.get("late_pct", ""),
            "max_late_us": m_val.get("max_late_us", ""),
            "cum_late_us": m_val.get("cum_late_us", ""),
            "max_consec_late": m_val.get("max_consec_late", ""),
            "total_fir_us": k_val.get("total_fir_us", ""),
            "total_ola_us": k_val.get("total_ola_us", ""),
            "cache_hit_pct": r_val.get("hit_pct", ""),
            "cache_sample_hit_pct": r_val.get("sample_hit_pct", ""),
            "fir_saved_us": r_val.get("fir_saved_us", ""),
            "source_reuse_pct": s_val.get("reuse_pct", ""),
            "slack_ge_1_pct": sl_val.get("ge_1_pct", ""),
            "audio_pass": c_val.get("audio_pass", ""),
            "transport_pass": c_val.get("transport_pass", "")
        })

    # Stability rows
    for sname in ["stability_harmonizer_60s", "stability_worst_synth_60s"]:
        c_val = cases_dict.get(sname, {})
        m_val = metrics_dict.get(sname, {})
        r_val = residual_stats_dict.get(sname, {})
        if m_val:
            stability_rows.append({
                "name": sname,
                "blocks": c_val.get("blocks", ""),
                "wall_s": c_val.get("wall_s", ""),
                "avg_us": m_val.get("avg_us", ""),
                "p50_us": m_val.get("p50_us", ""),
                "p90_us": m_val.get("p90_us", ""),
                "p95_us": m_val.get("p95_us", ""),
                "p99_us": m_val.get("p99_us", ""),
                "max_us": m_val.get("max_us", ""),
                "misses": m_val.get("misses", ""),
                "late_pct": m_val.get("late_pct", ""),
                "hit_pct": r_val.get("hit_pct", ""),
                "sample_hit_pct": r_val.get("sample_hit_pct", ""),
                "fir_saved_us": r_val.get("fir_saved_us", "")
            })

    # Write CSVs
    # 1. Capacity sweep
    cap_cols = [
        "N", "hit_rate", "sample_hit_rate", "lookups", "lookup_us", "evictions",
        "fir_saved_us", "avg_us", "p99_us", "max_us", "misses", "bytes_voice",
        "is_psram", "dist1", "dist2", "dist3", "dist4", "dist5", "dist6", "dist7", "dist8", "dist9_16", "dist_gt16"
    ]
    write_csv(OUT / "b4c3b_cache_capacity_sweep.csv", capacity_rows, cap_cols)

    # 2. Candidate comparison
    cand_cols = [
        "idx", "name", "avg_mean", "avg_std", "p99_mean", "p99_std", "max_mean", "max_std",
        "misses_mean", "misses_std", "fir_mean", "fir_std", "ola_mean", "ola_std",
        "hit_mean", "hit_std"
    ]
    write_csv(OUT / "b4c3b_candidate_comparison.csv", candidates_rows, cand_cols)

    # 3. Full matrix
    matrix_cols = [
        "case_name", "suite", "nominal_f0", "blocks", "wall_s", "avg_us", "p50_us", "p90_us",
        "p95_us", "p99_us", "p999_us", "max_us", "cpu_pct", "margin_us", "misses", "late_pct",
        "max_late_us", "cum_late_us", "max_consec_late", "total_fir_us", "total_ola_us",
        "cache_hit_pct", "cache_sample_hit_pct", "fir_saved_us", "source_reuse_pct",
        "slack_ge_1_pct", "audio_pass", "transport_pass"
    ]
    write_csv(OUT / "b4c3b_full_matrix.csv", full_matrix_rows, matrix_cols)

    # 4. Top 100 blocks
    top100_cols = [
        "rank", "block_index", "runtime_us", "base_us", "v0_us", "v1_us", "warp_us",
        "sched_v0", "sched_v1", "rend_v0", "rend_v1", "samples", "fir_samples", "fir_reused",
        "hits_v0", "hits_v1", "miss_v0", "miss_v1", "ov_v0", "ov_v1", "slack_blocks", "recovery", "pitch_changed"
    ]
    write_csv(OUT / "b4c3b_top100_blocks.csv", top100_rows, top100_cols)

    # 5. Burst regression
    regr_cols = ["metric", "r2"]
    write_csv(OUT / "b4c3b_burst_regression.csv", regression_rows, regr_cols)

    # 6. Stability 60s
    stab_cols = [
        "name", "blocks", "wall_s", "avg_us", "p50_us", "p90_us", "p95_us", "p99_us",
        "max_us", "misses", "late_pct", "hit_pct", "sample_hit_pct", "fir_saved_us"
    ]
    write_csv(OUT / "b4c3b_stability_60s.csv", stability_rows, stab_cols)

    # 7. Memory footprint
    buf_cols = ["name", "bytes", "is_sram", "is_psram"]
    write_csv(OUT / "b4c3b_memory_footprint.csv", buffer_rows, buf_cols)

    # 8. Materialize Markdown Report
    start_idx = 0
    for i, line in enumerate(lines):
        if "STAGE B4C.3B — EXACT SOURCE-GRAIN REUSE & GRAIN-BURST FLATTENING AUDIT" in line:
            start_idx = i
            break
    audit_lines = lines[start_idx:] if start_idx < len(lines) else lines

    report = [
        "# Stage B4C.3B — Exact Source-Grain Reuse & Grain-Burst Flattening Audit Report",
        "",
        "**Target Hardware**: Wireless-Tag WT9932P4-TINY (ESP32-P4 @ 360 MHz)",
        f"**Timestamp**: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        "",
        "---",
        "",
        "```text",
    ]
    report.extend(audit_lines)
    report.append("```\n")

    report_path = OUT / "b4c3b_source_grain_burst_report.md"
    report_path.write_text("\n".join(report), encoding="utf-8")
    print(f"Generated comprehensive report: {report_path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=950.0)
    parser.add_argument("--skip-flash", action="store_true")
    args = parser.parse_args()

    if not args.skip_flash:
        flash(args.port)

    lines = capture(args.port, args.timeout)
    process_serial_log(lines)
    print("\nStage B4C.3B Execution and Artifact Generation Complete!\n")


if __name__ == "__main__":
    main()
