"""Stage B4C.3A Profiler Integrity & Grain-Burst Workload Audit Monitor & Artifact Generator.

Flashes firmware to ESP32-P4 on COM11, captures serial telemetry,
and generates all 8 CSVs, 4 raw TXTs, and the comprehensive markdown report in artifacts/alpha01c/.
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


def capture(port: str, timeout: float = 750.0, baud: int = 115200) -> list[str]:
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
            if "B4C.3A PROFILER INTEGRITY RESULT:" in line or "NEXT STEP:" in line:
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

    # Save raw serial log
    raw_serial_file = OUT / "b4c3a_raw_serial.txt"
    raw_serial_file.write_text("\n".join(lines), encoding="utf-8")
    print(f"Saved: {raw_serial_file}")

    # Top 100 blocks raw text
    top100_raw_lines = []
    # Calibration raw text
    calibration_lines = []
    # Effective config raw text
    effective_config_lines = []

    # Parsed structured data
    candidates_rows = []
    subsections_v0_rows = []
    subsections_v1_rows = []
    model_warp_rows = []
    grain_reuse_rows = []
    slack_rows = []
    top100_rows = []
    cases_dict = {}  # case_name -> B4C3A_CASE dict
    metrics_dict = {}  # case_name -> B4C3A_BLOCK_METRICS dict

    capture_effective_cfg = False

    for line in lines:
        if "AUDIO HARDWARE CONFIGURATION" in line:
            capture_effective_cfg = True
        elif capture_effective_cfg and "=================================================================" in line:
            capture_effective_cfg = False

        if capture_effective_cfg:
            effective_config_lines.append(line)

        if "B4C3A_PROFILER_MICROBENCH" in line or "PROFILER OVERHEAD PER BLOCK:" in line:
            calibration_lines.append(line)

        if line.startswith("B4C3A_CANDIDATE_SUMMARY "):
            vals = parse_key_value_line("B4C3A_CANDIDATE_SUMMARY", line)
            row = {"candidate_id": vals.get("id", ""), "candidate_name": vals.get("name", "")}
            for k in ["avg", "p99", "max", "misses", "warp_us", "hit_rate", "exp_calls_blk"]:
                val_str = vals.get(k, "")
                if "±" in val_str:
                    m, s = val_str.replace("%", "").split("±", 1)
                    row[f"{k}_mean"] = m
                    row[f"{k}_std"] = s
                else:
                    row[f"{k}_mean"] = val_str.replace("%", "")
                    row[f"{k}_std"] = "0.0"
            candidates_rows.append(row)

        elif line.startswith("B4C3A_CASE "):
            vals = parse_key_value_line("B4C3A_CASE", line)
            cases_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3A_BLOCK_METRICS "):
            vals = parse_key_value_line("B4C3A_BLOCK_METRICS", line)
            metrics_dict[vals.get("name", "")] = vals

        elif line.startswith("B4C3A_SUBSECTION "):
            vals = parse_key_value_line("B4C3A_SUBSECTION", line)
            if vals.get("voice") == "0":
                subsections_v0_rows.append(vals)
            else:
                subsections_v1_rows.append(vals)

        elif line.startswith("B4C3A_MODEL_WARP_DECOMPOSED "):
            clean_line = line.replace(" | ", " ")
            vals = parse_key_value_line("B4C3A_MODEL_WARP_DECOMPOSED", clean_line)
            model_warp_rows.append(vals)

        elif line.startswith("B4C3A_SOURCE_GRAIN_AUDIT "):
            vals = parse_key_value_line("B4C3A_SOURCE_GRAIN_AUDIT", line)
            grain_reuse_rows.append(vals)

        elif line.startswith("B4C3A_SCHEDULING_SLACK "):
            vals = parse_key_value_line("B4C3A_SCHEDULING_SLACK", line)
            slack_rows.append(vals)

        elif line.startswith("B4C3A_TOP100_ROW "):
            top100_raw_lines.append(line)
            tokens = line[len("B4C3A_TOP100_ROW "):].strip().split(",")
            if len(tokens) >= 20:
                top100_rows.append({
                    "rank": tokens[0], "block_index": tokens[1], "runtime_us": tokens[2],
                    "base_us": tokens[3], "v0_us": tokens[4], "v1_us": tokens[5],
                    "warp_us": tokens[6], "sched_v0": tokens[7], "sched_v1": tokens[8],
                    "rend_v0": tokens[9], "rend_v1": tokens[10], "samples": tokens[11],
                    "uniq_samples": tokens[12], "uniq_keys": tokens[13], "dup_keys": tokens[14],
                    "warp_lookups": tokens[15], "warp_exp": tokens[16], "slack_blocks": tokens[17],
                    "recovery": tokens[18], "pitch_changed": tokens[19]
                })

    # Save raw text files
    (OUT / "b4c3a_top100_blocks_raw.txt").write_text("\n".join(top100_raw_lines), encoding="utf-8")
    (OUT / "b4c3a_profiler_calibration_raw.txt").write_text("\n".join(calibration_lines), encoding="utf-8")
    (OUT / "b4c3a_effective_config_raw.txt").write_text("\n".join(effective_config_lines), encoding="utf-8")

    # Write CSV 1: Candidate Comparison
    cand_cols = [
        "candidate_id", "candidate_name", "avg_mean", "avg_std", "p99_mean", "p99_std",
        "max_mean", "max_std", "misses_mean", "misses_std", "warp_us_mean", "warp_us_std",
        "hit_rate_mean", "hit_rate_std", "exp_calls_blk_mean", "exp_calls_blk_std"
    ]
    write_csv(OUT / "b4c3a_candidate_comparison.csv", candidates_rows, cand_cols)

    # Write CSV 2 & 3: Subsections V0 & V1
    sec_cols = ["case", "voice", "sec_idx", "sec_name", "us_block", "us_call", "calls_block", "calls", "cycles"]
    write_csv(OUT / "b4c3a_subsections_v0.csv", subsections_v0_rows, sec_cols)
    write_csv(OUT / "b4c3a_subsections_v1.csv", subsections_v1_rows, sec_cols)

    # Write CSV 4: Model Warp Decomposed
    mw_cols = [
        "name",
        "v0_near_calls", "v0_near_us", "v0_lam_calls", "v0_lam_us", "v0_lookup_calls", "v0_lookup_us",
        "v0_hit_calls", "v0_hit_us", "v0_miss_calls", "v0_miss_us", "v0_poly_calls", "v0_poly_us",
        "v0_gn_calls", "v0_gn_us", "v0_copy_calls", "v0_copy_us", "v0_loc", "v0_sh", "v0_neut", "v0_exp",
        "v1_near_calls", "v1_near_us", "v1_lam_calls", "v1_lam_us", "v1_lookup_calls", "v1_lookup_us",
        "v1_hit_calls", "v1_hit_us", "v1_miss_calls", "v1_miss_us", "v1_poly_calls", "v1_poly_us",
        "v1_gn_calls", "v1_gn_us", "v1_copy_calls", "v1_copy_us", "v1_loc", "v1_sh", "v1_neut", "v1_exp"
    ]
    write_csv(OUT / "b4c3a_model_warp_decomposed.csv", model_warp_rows, mw_cols)

    # Write CSV 5: Source Grain Reuse
    reuse_cols = [
        "name", "requested", "unique", "duplicate", "same_voice", "cross_voice",
        "total_samples", "reusable_samples", "reuse_pct", "same_pct", "cross_pct", "sample_weighted_pct"
    ]
    write_csv(OUT / "b4c3a_source_grain_reuse.csv", grain_reuse_rows, reuse_cols)

    # Write CSV 6: Scheduling Slack
    slack_cols = [
        "name", "total_grains", "p0", "p10", "p50", "p90", "p99", "max",
        "ge_1_pct", "ge_2_pct", "ge_4_pct", "ge_8_pct"
    ]
    write_csv(OUT / "b4c3a_scheduling_slack.csv", slack_rows, slack_cols)

    # Write CSV 7: Top 100 Slowest Blocks
    top100_cols = [
        "rank", "block_index", "runtime_us", "base_us", "v0_us", "v1_us", "warp_us",
        "sched_v0", "sched_v1", "rend_v0", "rend_v1", "samples", "uniq_samples",
        "uniq_keys", "dup_keys", "warp_lookups", "warp_exp", "slack_blocks", "recovery", "pitch_changed"
    ]
    write_csv(OUT / "b4c3a_top100_slowest_blocks.csv", top100_rows, top100_cols)

    # Write CSV 8: Frequency Matrix
    freq_cases = ["f0_65_extreme_bass", "f0_80_bass", "f0_110_a2", "f0_147_d3", "f0_220_a3", "f0_440_a4"]
    freq_rows = []
    reuse_by_case = {r.get("name"): r for r in grain_reuse_rows}
    slack_by_case = {r.get("name"): r for r in slack_rows}

    for fc in freq_cases:
        c_val = cases_dict.get(fc, {})
        m_val = metrics_dict.get(fc, {})
        r_val = reuse_by_case.get(fc, {})
        s_val = slack_by_case.get(fc, {})
        freq_rows.append({
            "case": fc,
            "f0_hz": c_val.get("f0", ""),
            "blocks": c_val.get("blocks", ""),
            "avg_us": m_val.get("avg_us", ""),
            "p50_us": m_val.get("p50_us", ""),
            "p90_us": m_val.get("p90_us", ""),
            "p95_us": m_val.get("p95_us", ""),
            "p99_us": m_val.get("p99_us", ""),
            "max_us": m_val.get("max_us", ""),
            "misses": m_val.get("misses", ""),
            "late_pct": m_val.get("late_pct", ""),
            "reuse_pct": r_val.get("reuse_pct", ""),
            "slack_ge_1_pct": s_val.get("ge_1_pct", "")
        })

    freq_cols = [
        "case", "f0_hz", "blocks", "avg_us", "p50_us", "p90_us", "p95_us", "p99_us",
        "max_us", "misses", "late_pct", "reuse_pct", "slack_ge_1_pct"
    ]
    write_csv(OUT / "b4c3a_frequency_matrix.csv", freq_rows, freq_cols)

    # Materialize Markdown Report
    start_idx = 0
    for i, line in enumerate(lines):
        if "STAGE B4C.3A — PROFILER INTEGRITY & GRAIN-BURST WORKLOAD AUDIT" in line:
            start_idx = i
            break
    audit_lines = lines[start_idx:] if start_idx < len(lines) else lines

    report = [
        "# Stage B4C.3A — Profiler Integrity & Grain-Burst Workload Audit Report",
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

    report_path = OUT / "b4c3a_profiler_integrity_report.md"
    report_path.write_text("\n".join(report), encoding="utf-8")
    print(f"Generated comprehensive report: {report_path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=750.0)
    parser.add_argument("--skip-flash", action="store_true")
    args = parser.parse_args()

    if not args.skip_flash:
        flash(args.port)

    lines = capture(args.port, args.timeout)
    process_serial_log(lines)
    print("\nStage B4C.3A Execution and Artifact Generation Complete!\n")


if __name__ == "__main__":
    main()
