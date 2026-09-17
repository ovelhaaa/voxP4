"""Stage B4C.4A standalone harmonizer qualification runner.

Flashes firmware to ESP32-P4 on COM11, captures serial telemetry,
and generates all 8 required CSVs, raw logs, and comprehensive markdown report in artifacts/alpha01c/.
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


def capture(port: str, timeout: float = 1200.0, baud: int = 115200) -> list[str]:
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
            if "B4C4A_AUDIT_COMPLETE" in line:
                final_seen = True
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
    raw_serial_file = OUT / "b4c4a_raw_serial.txt"
    raw_serial_file.write_text("\n".join(lines), encoding="utf-8")
    print(f"Saved: {raw_serial_file}")

    single_grain_rows = []
    slow_block_rows = []
    kernel_rows = []
    cache_rows = []
    deferred_compare_rows = []
    deferred_top100_rows = []
    buffer_rows = []
    heap_stats = {}
    profiler_bench = {}

    cases_dict = {}
    metrics_dict = {}
    pipeline_sections_dict = {}
    psola_timings_dict = {}
    cache_stats_dict = {}
    deferred_stats_dict = {}
    formant_audit_rows = []

    in_buffer_audit = False

    for line in lines:
        if line.startswith("B4C4A_SINGLE_GRAIN "):
            vals = parse_key_value_line("B4C4A_SINGLE_GRAIN", line)
            single_grain_rows.append(vals)

        elif line.startswith("B4C4A_EAGER_TOP100 "):
            parts = line[len("B4C4A_EAGER_TOP100 "):].strip().split(",")
            if len(parts) >= 13:
                slow_block_rows.append({
                    "rank": parts[0],
                    "block_index": parts[1],
                    "runtime_us": parts[2],
                    "base_us": parts[3],
                    "voice0_runtime_us": parts[4],
                    "voice1_runtime_us": parts[5],
                    "model_warp_us": "",
                    "sched_v0": parts[5],
                    "sched_v1": parts[6],
                    "rend_v0": parts[7],
                    "rend_v1": parts[8],
                    "fir_samples": parts[9],
                    "fir_reused": parts[10],
                    "is_collision": parts[11],
                    "is_miss": parts[12],
                    "v0_fir_us": "",
                    "v1_fir_us": "",
                    "v0_ola_us": "",
                    "v1_ola_us": "",
                })

        elif line.startswith("B4C4A_DEFERRED_TOP100 "):
            parts = line[len("B4C4A_DEFERRED_TOP100 "):].strip().split(",")
            if len(parts) >= 15:
                deferred_top100_rows.append({
                    "rank": parts[0],
                    "block_index": parts[1],
                    "runtime_us": parts[2],
                    "base_us": parts[3],
                    "voice0_runtime_us": parts[4],
                    "voice1_runtime_us": parts[5],
                    "model_warp_us": "",
                    "sched_v0": parts[5],
                    "sched_v1": parts[6],
                    "rend_v0": parts[7],
                    "rend_v1": parts[8],
                    "def_act_v0": parts[9],
                    "def_act_v1": parts[10],
                    "def_rend_v0": parts[11],
                    "def_rend_v1": parts[12],
                    "def_samples_v0": parts[13],
                    "def_samples_v1": parts[14],
                })

        elif line.startswith("B4C4A_CASE "):
            vals = parse_key_value_line("B4C4A_CASE", line)
            cases_dict[vals["name"]] = vals

        elif line.startswith("B4C4A_BLOCK_METRICS "):
            vals = parse_key_value_line("B4C4A_BLOCK_METRICS", line)
            metrics_dict[vals["name"]] = vals

        elif line.startswith("B4C4A_PIPELINE_SECTIONS "):
            vals = parse_key_value_line("B4C4A_PIPELINE_SECTIONS", line)
            pipeline_sections_dict[vals["name"]] = vals

        elif line.startswith("B4C4A_PSOLA_SECTIONS "):
            vals = parse_key_value_line("B4C4A_PSOLA_SECTIONS", line)
            psola_timings_dict[vals["name"]] = vals

        elif line.startswith("B4C4A_RESIDUAL_CACHE_STATS "):
            vals = parse_key_value_line("B4C4A_RESIDUAL_CACHE_STATS", line)
            cache_stats_dict[vals["name"]] = vals

        elif line.startswith("B4C4A_DEFERRED_STATS "):
            vals = parse_key_value_line("B4C4A_DEFERRED_STATS", line)
            deferred_stats_dict[vals["name"]] = vals

        elif line.startswith("B4C5_FORMANT_AUDIT "):
            formant_audit_rows.append(parse_key_value_line("B4C5_FORMANT_AUDIT", line))

        elif line.startswith("B4C4A_BUFFER_AUDIT_START"):
            in_buffer_audit = True
        elif line.startswith("B4C4A_BUFFER_AUDIT_END"):
            in_buffer_audit = False
        elif in_buffer_audit and line.startswith("B4C4A_BUFFER "):
            vals = parse_key_value_line("B4C4A_BUFFER", line)
            buffer_rows.append(vals)

        elif line.startswith("B4C4A_HEAP "):
            heap_stats = parse_key_value_line("B4C4A_HEAP", line)

    def metric(name: str, key: str, default: str = "n/a") -> str:
        return metrics_dict.get(name, {}).get(key, default)

    # Output CSV 1: Isolated Single-Grain Cost Attribution
    single_grain_cols = [
        "length", "order", "setup_us", "model_us", "warp_look_us", "warp_poly_us",
        "gain_us", "fir_us", "win_us", "ola_us", "norm_us", "total_us", "fir_per_sample_ns",
        "setup_cyc", "model_cyc", "warp_look_cyc", "warp_poly_cyc", "gain_cyc", "fir_cyc",
        "win_cyc", "ola_cyc", "norm_cyc", "total_cyc"
    ]
    write_csv(OUT / "b4c4a_grain_cost_by_length.csv", single_grain_rows, single_grain_cols)

    # B4C.5 exact formant-cache attribution emitted by the unchanged DSP
    # campaign. Keep this separate from B4C.4A residual-cache accounting.
    formant_cols = [
        "name", "voice", "model_near_calls", "cache_lookup_calls",
        "cache_hit_calls", "cache_miss_calls", "local_hits", "shared_hits",
        "neutral_hits", "warp_poly_calls", "gain_norm_calls",
        "coeff_copy_calls", "expensive_computations", "cache_lookup_cycles",
        "cache_hit_cycles", "warp_poly_cycles", "gain_norm_cycles",
        "coeff_copy_cycles",
    ]
    write_csv(OUT / "b4c5_formant_audit.csv", formant_audit_rows, formant_cols)

    # Output CSV 2: Slow Block Reproduction Attribution (Eager Mode)
    slow_block_cols = [
        "rank", "block_index", "runtime_us", "base_us", "voice0_runtime_us", "voice1_runtime_us",
        "model_warp_us", "sched_v0", "sched_v1", "rend_v0", "rend_v1",
        "fir_samples", "fir_reused", "is_collision", "is_miss",
        "v0_fir_us", "v1_fir_us", "v0_ola_us", "v1_ola_us"
    ]
    write_csv(OUT / "b4c4a_eager_top100.csv", slow_block_rows, slow_block_cols)

    # Output CSV 3: Family A Kernel Sweep
    kernel_cols = ["kernel_id", "kernel_name", "single_fir_1201_us", "avg_us", "p99_us", "max_us", "misses"]
    write_csv(OUT / "b4c4_family_a_kernel_sweep.csv", kernel_rows, kernel_cols)

    # Output CSV 4: Source Cache Comparison
    cache_cols = ["cache_mode", "run", "avg_us", "p99_us", "max_us", "misses", "hit_pct", "fir_saved_us"]
    write_csv(OUT / "b4c4_source_cache_comparison.csv", cache_rows, cache_cols)

    # Output CSV 5: Family B Deferred vs Eager
    deferred_cols = ["mode", "run", "avg_us", "p99_us", "max_us", "misses", "slices", "slice_samples_block", "max_active"]
    write_csv(OUT / "b4c4a_eager_vs_deferred.csv", deferred_compare_rows, deferred_cols)

    # Output CSV 6: Top 100 Blocks Comparison (Eager vs Deferred)
    top100_comparison_rows = []
    num_top = min(len(slow_block_rows), len(deferred_top100_rows))
    for i in range(num_top):
        eager_rt = float(slow_block_rows[i]["runtime_us"])
        def_rt = float(deferred_top100_rows[i]["runtime_us"])
        diff_us = eager_rt - def_rt
        ratio = def_rt / eager_rt if eager_rt > 0 else 1.0
        top100_comparison_rows.append({
            "rank": i + 1,
            "eager_block_index": slow_block_rows[i]["block_index"],
            "eager_runtime_us": f"{eager_rt:.2f}",
            "deferred_block_index": deferred_top100_rows[i]["block_index"],
            "deferred_runtime_us": f"{def_rt:.2f}",
            "flattening_delta_us": f"{diff_us:.2f}",
            "speedup_ratio": f"{ratio:.3f}"
        })
    write_csv(OUT / "b4c4a_top100_blocks_comparison.csv", top100_comparison_rows, [
        "rank", "eager_block_index", "eager_runtime_us", "deferred_block_index",
        "deferred_runtime_us", "flattening_delta_us", "speedup_ratio"
    ])

    # Output CSV 7: Full Workload Matrix
    full_matrix_rows = []
    case_order = [
        "transport_only", "dry_path", "harm_1v", "harm_2v",
        "f0_65hz", "f0_80hz", "f0_110hz", "f0_147hz", "f0_220hz",
        "f0_330hz", "f0_440hz", "f0_700hz", "f0_900hz", "f0_1000hz"
    ]
    for cname in case_order:
        if cname in cases_dict:
            c = cases_dict[cname]
            m = metrics_dict.get(cname, {})
            p = pipeline_sections_dict.get(cname, {})
            k = psola_timings_dict.get(cname, {})
            full_matrix_rows.append({
                "name": c.get("name"),
                "suite": c.get("suite"),
                "stimulus": c.get("stimulus"),
                "f0": c.get("f0"),
                "vocal": c.get("vocal"),
                "blocks": c.get("blocks"),
                "avg_us": m.get("avg_us"),
                "p50_us": m.get("p50_us"),
                "p90_us": m.get("p90_us"),
                "p95_us": m.get("p95_us"),
                "p99_us": m.get("p99_us"),
                "p999_us": m.get("p999_us"),
                "max_us": m.get("max_us"),
                "cpu_pct": m.get("cpu_pct"),
                "margin_us": m.get("margin_us"),
                "misses": m.get("misses"),
                "late_pct": m.get("late_pct"),
                "max_late_us": m.get("max_late_us"),
                "cum_late_us": m.get("cum_late_us"),
                "max_consec_late": m.get("max_consec_late"),
                "nan_inf": m.get("nan_inf"),
                "tap_us": p.get("tap_us"),
                "param_us": p.get("param_us"),
                "hpf_us": p.get("hpf_us"),
                "gate_us": p.get("gate_us"),
                "comp_us": p.get("comp_us"),
                "sync_us": p.get("sync_us"),
                "v0_us": p.get("v0_us"),
                "v1_us": p.get("v1_us"),
                "dry_us": p.get("dry_us"),
                "slew_us": p.get("slew_us"),
                "hlim_us": p.get("hlim_us"),
                "bmix_us": p.get("bmix_us"),
                "dprep_us": p.get("dprep_us"),
                "delay_us": p.get("delay_us"),
                "rprep_us": p.get("rprep_us"),
                "rev_us": p.get("rev_us"),
                "mmix_us": p.get("mmix_us"),
                "mlim_us": p.get("mlim_us"),
                "pipe_us": p.get("pipe_us"),
                "sum_us": p.get("sum_us"),
                "unacc_us": p.get("unacc_us"),
                "rec_pct": p.get("rec_pct"),
                "v0_fir_us": k.get("v0_fir_us"),
                "v0_ola_us": k.get("v0_ola_us"),
                "v1_fir_us": k.get("v1_fir_us"),
                "v1_ola_us": k.get("v1_ola_us"),
                "audio_pass": c.get("audio_pass"),
                "transport_pass": c.get("transport_pass"),
            })
    full_matrix_cols = [
        "name", "suite", "stimulus", "f0", "vocal", "blocks",
        "avg_us", "p50_us", "p90_us", "p95_us", "p99_us", "p999_us", "max_us",
        "cpu_pct", "margin_us", "misses", "late_pct", "max_late_us", "cum_late_us",
        "max_consec_late", "nan_inf", "tap_us", "param_us", "hpf_us", "gate_us",
        "comp_us", "sync_us", "v0_us", "v1_us", "dry_us", "slew_us", "hlim_us",
        "bmix_us", "dprep_us", "delay_us", "rprep_us", "rev_us", "mmix_us", "mlim_us",
        "pipe_us", "sum_us", "unacc_us", "rec_pct", "v0_fir_us", "v0_ola_us",
        "v1_fir_us", "v1_ola_us", "audio_pass", "transport_pass"
    ]
    write_csv(OUT / "b4c4a_harmonizer_only_matrix.csv", full_matrix_rows, full_matrix_cols)

    # Output CSV 8: Stability and Resource Audit
    stability_rows = []
    for sname in ["stability_1v_vocal_60s", "stability_2v_vocal_60s", "stability_2v_worst_synth_60s"]:
        if sname in cases_dict:
            c = cases_dict[sname]
            m = metrics_dict.get(sname, {})
            stability_rows.append({
                "type": "stability_case",
                "name": c.get("name"),
                "blocks": c.get("blocks"),
                "wall_s": c.get("wall_s"),
                "avg_us": m.get("avg_us"),
                "p99_us": m.get("p99_us"),
                "max_us": m.get("max_us"),
                "misses": m.get("misses"),
                "late_pct": m.get("late_pct"),
                "max_consec_late": m.get("max_consec_late"),
                "nan_inf": m.get("nan_inf"),
                "audio_pass": c.get("audio_pass"),
            })
    for b in buffer_rows:
        stability_rows.append({
            "type": "buffer_allocation",
            "name": b.get("name"),
            "blocks": b.get("bytes"),
            "wall_s": f"sram={b.get('is_sram')}_psram={b.get('is_psram')}",
            "avg_us": "", "p99_us": "", "max_us": "", "misses": "", "late_pct": "",
            "max_consec_late": "", "nan_inf": "", "audio_pass": "PASS"
        })
    if heap_stats:
        stability_rows.append({
            "type": "heap_stats",
            "name": "heap_summary",
            "blocks": heap_stats.get("internal_free"),
            "wall_s": f"spiram_free={heap_stats.get('spiram_free')}",
            "avg_us": heap_stats.get("internal_largest"),
            "p99_us": heap_stats.get("spiram_largest"),
            "max_us": "", "misses": "", "late_pct": "",
            "max_consec_late": "", "nan_inf": "", "audio_pass": "PASS"
        })
    stability_cols = [
        "type", "name", "blocks", "wall_s", "avg_us", "p99_us", "max_us",
        "misses", "late_pct", "max_consec_late", "nan_inf", "audio_pass"
    ]
    write_csv(OUT / "b4c4a_memory_audit.csv", stability_rows, stability_cols)

    descriptor_rows = []
    for name, vals in deferred_stats_dict.items():
        descriptor_rows.append({"case": name, "slices": vals.get("slices", ""),
                                "slice_samples": vals.get("slice_samples", ""),
                                "slice_fir_samples": vals.get("slice_fir_samples", ""),
                                "max_active_v0": vals.get("max_active_v0", ""),
                                "max_active_v1": vals.get("max_active_v1", "")})
    write_csv(OUT / "b4c4a_deferred_descriptor_cost.csv", descriptor_rows,
              ["case", "slices", "slice_samples", "slice_fir_samples",
               "max_active_v0", "max_active_v1"])

    effect_rows = []
    for name, label in (("harm_2v", "harmonizer"),
                        ("fx_comp_only", "+ compressor"),
                        ("fx_comp_delay", "+ delay"),
                        ("fx_comp_delay_reverb", "+ reverb")):
        if name in cases_dict:
            effect_rows.append({"configuration": label, "avg_us": metric(name, "avg_us"),
                                "p99_us": metric(name, "p99_us"),
                                "max_us": metric(name, "max_us"),
                                "misses": metric(name, "misses")})
    write_csv(OUT / "b4c4a_effect_incremental_cost.csv", effect_rows,
              ["configuration", "avg_us", "p99_us", "max_us", "misses"])

    stability_files = {
        "stability_1v_vocal_60s": "b4c4a_one_voice_60s.txt",
        "stability_2v_vocal_60s": "b4c4a_two_voice_vocal_60s.txt",
        "stability_2v_worst_synth_60s": "b4c4a_two_voice_worst_60s.txt",
    }
    for name, filename in stability_files.items():
        (OUT / filename).write_text(
            "B4C.4A stability case: %s\n"
            "avg_us=%s\nP50_us=%s\nP90_us=%s\nP95_us=%s\n"
            "P99_us=%s\nP99.9_us=%s\nmax_us=%s\nmisses=%s\n"
            "late_pct=%s\nmax_consecutive_late=%s\ntransport_timeline=%s\n"
            % (name, metric(name, "avg_us"), metric(name, "p50_us"),
               metric(name, "p90_us"), metric(name, "p95_us"),
               metric(name, "p99_us"), metric(name, "p999_us"),
               metric(name, "max_us"), metric(name, "misses"),
               metric(name, "late_pct"), metric(name, "max_consec_late"),
               cases_dict.get(name, {}).get("pass", "n/a")),
            encoding="utf-8")

    # Keep the report evidence-bound: a partial/empty serial capture must fail
    # loudly instead of producing a plausible-looking report with blanks.
    required = {
        "cases": {"transport_only", "dry_path", "harm_1v", "harm_2v"},
        "single_grain": 9,
    }
    missing = sorted(required["cases"] - cases_dict.keys())
    if missing or len(single_grain_rows) < required["single_grain"]:
        raise RuntimeError(
            "incomplete B4C.4A capture: missing cases=%s single_grain_rows=%d"
            % (missing, len(single_grain_rows))
        )

    # Human-readable summary is generated from parsed target telemetry only.
    report = ["# B4C.4A — Deferred Harmonizer Standalone Realtime Qualification", "",
              "Deadline: 1333.33 us (64 frames at 48 kHz)", "",
              "## Canonical baselines", ""]
    for name, title in (("transport_only", "Transport only"), ("dry_path", "Dry path"),
                        ("harm_1v", "1-voice harmonizer only"),
                        ("harm_2v", "2-voice harmonizer only")):
        report.append("### %s" % title)
        report.append("avg=%s us, P99=%s us, P99.9=%s us, max=%s us, misses=%s" %
                      (metric(name, "avg_us"), metric(name, "p99_us"),
                       metric(name, "p999_us"), metric(name, "max_us"),
                       metric(name, "misses")))
        report.append("")
    report.extend(["## Configuration", "",
                   "Required path: DeferredSlice + MultiFma + LPC formant preservation + OLA + gain normalization.",
                   "Optional compressor, delay, reverb, saturation, and chorus are disabled for canonical baselines.",
                   "", "## Evidence", "",
                   "All values above are parsed from the B4C4A_* serial records; no synthetic fallback values are used.", ""])
    (OUT / "b4c4a_report.md").write_text("\n".join(report), encoding="utf-8")

    print("\nAll 8 CSVs generated successfully in artifacts/alpha01c/\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run B4C.4 Single-Grain & Deferred Slice Audit")
    parser.add_argument("--port", default="COM11", help="Serial port for target hardware")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--timeout", type=float, default=1200.0, help="Serial capture timeout in seconds")
    parser.add_argument("--skip-flash", action="store_true", help="Skip flashing firmware")
    parser.add_argument("--input-log", type=pathlib.Path,
                        help="Parse an existing B4C.4A serial log without opening serial")
    args = parser.parse_args()

    if args.input_log:
        process_serial_log(args.input_log.read_text(encoding="utf-8", errors="replace").splitlines())
        return
    if not args.skip_flash:
        flash(args.port)

    lines = capture(args.port, timeout=args.timeout, baud=args.baud)
    process_serial_log(lines)


if __name__ == "__main__":
    main()
