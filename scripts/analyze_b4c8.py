#!/usr/bin/env python3
"""B4C.8 serial output parser and CSV generator.

Parses B4C8_* tagged serial output lines and generates:
  - b4c8_diag_vs_qual.csv      (§4: diagnostic overhead A/B)
  - b4c8_both_active_breakdown.csv (§12: measured ranking)
  - b4c8_other_breakdown.csv   (§7-8: per-voice Other decomposition)
  - b4c8_instrumentation_cost.csv (§6: profiler overhead)
  - b4c8_slow_blocks_1v.csv    (§15-16: slow blocks)
  - b4c8_slow_blocks_2v.csv
  - b4c8_burst_correlations.csv (§17: burst causality)
  - b4c8_cache_layout.csv      (§27: data layout)
  - b4c8_candidate_matrix.csv  (§20: optimization candidates)
  - b4c8_stall_events.txt      (§42: stall events)

Usage:
  python3 scripts/analyze_b4c8.py < serial_output.txt [--outdir artifacts/alpha01c/]
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class VoiceOther:
    voice: int = 0
    cat: int = 0
    cycles: int = 0


@dataclass
class SlowBlock:
    voice: int = 0
    duration_us: int = 0
    block_index: int = 0
    core: int = 0
    mask: int = 0
    grn0: int = 0
    grn1: int = 0
    desc0: int = 0
    desc1: int = 0
    slc0: int = 0
    slc1: int = 0
    src0: int = 0
    src1: int = 0
    fir0: int = 0
    fir1: int = 0
    ola: int = 0
    syn: int = 0
    f0: float = 0.0
    marks: int = 0
    model_chg: int = 0


@dataclass
class StallEvent:
    index: int = 0
    duration_us: int = 0
    block_index: int = 0
    core: int = 0
    mask: int = 0
    section: int = 0
    pitch_state: int = 0
    published: int = 0
    diag_ring: int = 0
    psram: int = 0
    i2s: int = 0


@dataclass
class QualifyResult:
    name: str = ""
    eff_fs: float = 0.0
    err_pct: float = 0.0
    fs_ok: bool = False
    dma_ok: bool = False
    miss_ok: bool = False
    backlog_ok: bool = False
    result: str = ""


@dataclass
class Snapshot:
    name: str = ""
    wall_s: float = 0.0
    expected_blocks: float = 0.0
    rx_blocks: float = 0.0
    tx_blocks: float = 0.0
    dsp_blocks: float = 0.0
    eff_fs: float = 0.0


OTHER_CAT_NAMES = [
    "A.deferred_descriptor_traversal",
    "B.active_descriptor_tests",
    "C.slice_geometry_clipping",
    "D.source_ring_index",
    "E.history_source_fetch_scaffolding",
    "F.per_sample_loop_control",
    "G.normalization_denom_guards",
    "H.ola_buffer_index_wrap",
    "I.synthesis_state_scaffolding",
    "J.formant_state_preparation",
    "K.wet_dry_gain_smoothing",
    "L.recovery_fallback_branch_tests",
    "M.crossfade_blend_mix",
    "N.telemetry_state_update",
    "O.diagnostic_trace_fill",
    "P.profiler_overhead",
    "Q.bounds_safety_checks",
    "R.other_unattributed",
]


def parse_lines(lines):
    """Parse B4C8_* tagged serial output lines."""
    other_cats = defaultdict(lambda: defaultdict(int))
    slow_blocks = defaultdict(list)
    stall_events = []
    qualify_results = []
    snapshots = []
    memory_lines = []
    lpc_gate_lines = []
    transport_lines = []
    trace_avg_lines = []
    build_mode = ""
    stability_line = ""
    cache_layout = []

    for line in lines:
        line = line.strip()

        # Build mode
        m = re.match(r"B4C8_BUILD_MODE=(\w+)", line)
        if m:
            build_mode = m.group(1)
            continue

        # Memory
        if line.startswith("B4C8_MEMORY "):
            memory_lines.append(line)
            continue

        # LPC gate
        if line.startswith("B4C8_LPC_GATE "):
            lpc_gate_lines.append(line)
            continue

        # Transport
        if line.startswith("B4C8_TRANSPORT "):
            transport_lines.append(line)
            continue

        # Trace average
        if line.startswith("B4C8_TRACE_AVG "):
            trace_avg_lines.append(line)
            continue

        # Cache layout
        if line.startswith("B4C8_CACHE_OBJECT "):
            cache_layout.append(line)
            continue

        # Stability
        if line.startswith("B4C8_STABILITY "):
            stability_line = line
            continue

        # Other category
        m = re.match(r"B4C8_OTHER_CAT voice=(\d+) cat=(\d+) cycles=(\d+)", line)
        if m:
            voice, cat, cycles = int(m.group(1)), int(m.group(2)), int(m.group(3))
            other_cats[voice][cat] = cycles
            continue

        # Other section (legacy)
        m = re.match(r"B4C8_OTHER_SECTION voice=(\d+) legacy_section=(\d+) cycles=(\d+)", line)
        if m:
            continue

        # Slow block
        m = re.match(
            r"B4C8_SLOW_BLOCK voice=(\d+) idx=\d+ dur_us=(\d+) blk=(\d+) "
            r"core=(\d+) mask=(\d+) grn0=(\d+) grn1=(\d+) "
            r"desc0=(\d+) desc1=(\d+) slc0=(\d+) slc1=(\d+) "
            r"src0=(\d+) src1=(\d+) fir0=(\d+) fir1=(\d+) "
            r"ola=(\d+) syn=(\d+) f0=([0-9.]+) marks=(\d+) model_chg=(\d+)",
            line,
        )
        if m:
            sb = SlowBlock(
                voice=int(m.group(1)),
                duration_us=int(m.group(2)),
                block_index=int(m.group(3)),
                core=int(m.group(4)),
                mask=int(m.group(5)),
                grn0=int(m.group(6)),
                grn1=int(m.group(7)),
                desc0=int(m.group(8)),
                desc1=int(m.group(9)),
                slc0=int(m.group(10)),
                slc1=int(m.group(11)),
                src0=int(m.group(12)),
                src1=int(m.group(13)),
                fir0=int(m.group(14)),
                fir1=int(m.group(15)),
                ola=int(m.group(16)),
                syn=int(m.group(17)),
                f0=float(m.group(18)),
                marks=int(m.group(19)),
                model_chg=int(m.group(20)),
            )
            slow_blocks[sb.voice].append(sb)
            continue

        # Stall event
        m = re.match(
            r"B4C8_STALL idx=(\d+) dur_us=(\d+) blk=(\d+) core=(\d+) "
            r"mask=(\d+) section=(\d+) pitch_state=(\d+) published=(\d+) "
            r"diag_ring=(\d+) psram=(\d+) i2s=(\d+)",
            line,
        )
        if m:
            ev = StallEvent(
                index=int(m.group(1)),
                duration_us=int(m.group(2)),
                block_index=int(m.group(3)),
                core=int(m.group(4)),
                mask=int(m.group(5)),
                section=int(m.group(6)),
                pitch_state=int(m.group(7)),
                published=int(m.group(8)),
                diag_ring=int(m.group(9)),
                psram=int(m.group(10)),
                i2s=int(m.group(11)),
            )
            stall_events.append(ev)
            continue

        # B4C6A_QUALIFY
        m = re.match(
            r"B4C6A_QUALIFY name=(\S+) eff_fs=([0-9.]+) err_pct=([0-9.]+) "
            r"fs_ok=(\d) dma_ok=(\d) miss_ok=(\d) backlog_ok=(\d) result=(\w+)",
            line,
        )
        if m:
            qr = QualifyResult(
                name=m.group(1),
                eff_fs=float(m.group(2)),
                err_pct=float(m.group(3)),
                fs_ok=m.group(4) == "1",
                dma_ok=m.group(5) == "1",
                miss_ok=m.group(6) == "1",
                backlog_ok=m.group(7) == "1",
                result=m.group(8),
            )
            qualify_results.append(qr)
            continue

        # B4C6A_CASE (snapshot)
        m = re.match(
            r"B4C6A_CASE name=(\S+) wall_s=([0-9.]+) .* effective_fs=([0-9.]+)",
            line,
        )
        if m:
            sn = Snapshot(
                name=m.group(1),
                wall_s=float(m.group(2)),
                eff_fs=float(m.group(3)),
            )
            snapshots.append(sn)
            continue

    return {
        "build_mode": build_mode,
        "other_cats": other_cats,
        "slow_blocks": slow_blocks,
        "stall_events": stall_events,
        "qualify_results": qualify_results,
        "snapshots": snapshots,
        "memory_lines": memory_lines,
        "lpc_gate_lines": lpc_gate_lines,
        "transport_lines": transport_lines,
        "trace_avg_lines": trace_avg_lines,
        "stability_line": stability_line,
        "cache_layout": cache_layout,
    }


def write_other_breakdown(data, outdir):
    """Write b4c8_other_breakdown.csv (§7-8)."""
    path = os.path.join(outdir, "b4c8_other_breakdown.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["voice", "category_index", "category_name", "cycles_total"])
        for voice in sorted(data["other_cats"].keys()):
            for cat in sorted(data["other_cats"][voice].keys()):
                name = (
                    OTHER_CAT_NAMES[cat] if cat < len(OTHER_CAT_NAMES) else f"cat_{cat}"
                )
                w.writerow([voice, cat, name, data["other_cats"][voice][cat]])
    print(f"  wrote {path}")


def write_slow_blocks(data, outdir):
    """Write b4c8_slow_blocks_*.csv (§15-16)."""
    for voice, blocks in data["slow_blocks"].items():
        label = "1v" if voice == 0 else "2v"
        path = os.path.join(outdir, f"b4c8_slow_blocks_{label}.csv")
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(
                [
                    "duration_us",
                    "block_index",
                    "core",
                    "voice_mask",
                    "new_grains_v0",
                    "new_grains_v1",
                    "active_desc_v0",
                    "active_desc_v1",
                    "deferred_slices_v0",
                    "deferred_slices_v1",
                    "source_samples_v0",
                    "source_samples_v1",
                    "fir_samples_v0",
                    "fir_samples_v1",
                    "ola_samples",
                    "synthesis_samples",
                    "pitch_f0",
                    "mark_count",
                    "model_changes",
                ]
            )
            for b in blocks:
                w.writerow(
                    [
                        b.duration_us,
                        b.block_index,
                        b.core,
                        b.mask,
                        b.grn0,
                        b.grn1,
                        b.desc0,
                        b.desc1,
                        b.slc0,
                        b.slc1,
                        b.src0,
                        b.src1,
                        b.fir0,
                        b.fir1,
                        b.ola,
                        b.syn,
                        f"{b.f0:.1f}",
                        b.marks,
                        b.model_chg,
                    ]
                )
        print(f"  wrote {path}")


def write_stall_events(data, outdir):
    """Write b4c8_stall_events.txt (§42)."""
    path = os.path.join(outdir, "b4c8_stall_events.txt")
    with open(path, "w") as f:
        f.write(f"# B4C.8 stall events (blocks >10 ms)\n")
        f.write(f"# count: {len(data['stall_events'])}\n\n")
        for ev in data["stall_events"]:
            f.write(
                f"STALL idx={ev.index} dur_us={ev.duration_us} blk={ev.block_index} "
                f"core={ev.core} mask={ev.mask} section={ev.section} "
                f"pitch_state={ev.pitch_state} published={ev.published} "
                f"diag_ring={ev.diag_ring} psram={ev.psram} i2s={ev.i2s}\n"
            )
    print(f"  wrote {path}")


def write_burst_correlations(data, outdir):
    """Write b4c8_burst_correlations.csv (§17)."""
    path = os.path.join(outdir, "b4c8_burst_correlations.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "voice",
                "duration_us",
                "new_grains_total",
                "active_desc_total",
                "deferred_slices_total",
                "source_samples_total",
                "fir_samples_total",
                "ola_samples",
                "synthesis_samples",
                "pitch_f0",
                "model_changes",
            ]
        )
        for voice, blocks in data["slow_blocks"].items():
            for b in blocks:
                w.writerow(
                    [
                        voice,
                        b.duration_us,
                        b.grn0 + b.grn1,
                        b.desc0 + b.desc1,
                        b.slc0 + b.slc1,
                        b.src0 + b.src1,
                        b.fir0 + b.fir1,
                        b.ola,
                        b.syn,
                        f"{b.f0:.1f}",
                        b.model_chg,
                    ]
                )
    print(f"  wrote {path}")


def write_cache_layout(data, outdir):
    """Write b4c8_cache_layout.csv (§27)."""
    path = os.path.join(outdir, "b4c8_cache_layout.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["object", "bytes", "placement"])
        for line in data["cache_layout"]:
            m = re.match(
                r"B4C8_CACHE_OBJECT name=(\S+) bytes=(\d+) placement=(\S+)", line
            )
            if m:
                w.writerow([m.group(1), m.group(2), m.group(3)])
    print(f"  wrote {path}")


def write_memory(data, outdir):
    """Write b4c8_memory.txt (§48)."""
    path = os.path.join(outdir, "b4c8_memory.txt")
    with open(path, "w") as f:
        for line in data["memory_lines"]:
            f.write(line + "\n")
    print(f"  wrote {path}")


def write_transport(data, outdir):
    """Write b4c8_transport.txt."""
    path = os.path.join(outdir, "b4c8_transport.txt")
    with open(path, "w") as f:
        for line in data["transport_lines"]:
            f.write(line + "\n")
    print(f"  wrote {path}")


def write_lpc_gate(data, outdir):
    """Append LPC order to report."""
    for line in data["lpc_gate_lines"]:
        print(f"  {line}")


def write_report(data, outdir):
    """Write b4c8_report.md."""
    path = os.path.join(outdir, "b4c8_report.md")
    with open(path, "w") as f:
        f.write("# B4C.8 Report\n\n")
        f.write(f"Build mode: {data['build_mode']}\n\n")

        # Qualification results
        f.write("## Qualification Results\n\n")
        for qr in data["qualify_results"]:
            f.write(f"- **{qr.name}**: Fs={qr.eff_fs:.2f} Hz, "
                    f"result={qr.result} "
                    f"(fs_ok={qr.fs_ok}, dma_ok={qr.dma_ok}, "
                    f"miss_ok={qr.miss_ok}, backlog_ok={qr.backlog_ok})\n")

        # Other breakdown
        f.write("\n## Voice Other Breakdown\n\n")
        f.write("| Voice | Category | Cycles |\n")
        f.write("|-------|----------|--------|\n")
        for voice in sorted(data["other_cats"].keys()):
            total_other = sum(data["other_cats"][voice].values())
            f.write(f"| {voice} | **TOTAL OTHER** | **{total_other}** |\n")
            for cat in sorted(data["other_cats"][voice].keys()):
                name = (
                    OTHER_CAT_NAMES[cat] if cat < len(OTHER_CAT_NAMES) else f"cat_{cat}"
                )
                f.write(f"| {voice} | {name} | {data['other_cats'][voice][cat]} |\n")

        # Slow blocks
        f.write("\n## Slow Blocks\n\n")
        for voice, blocks in sorted(data["slow_blocks"].items()):
            f.write(f"- Voice {voice}: {len(blocks)} blocks >1200 us\n")
            if blocks:
                max_dur = max(b.duration_us for b in blocks)
                avg_dur = sum(b.duration_us for b in blocks) / len(blocks)
                f.write(f"  - max: {max_dur} us, avg: {avg_dur:.0f} us\n")

        # Stall events
        f.write("\n## Stall Events (>10 ms)\n\n")
        f.write(f"- Count: {len(data['stall_events'])}\n")
        for ev in data["stall_events"][:10]:
            f.write(f"  - blk={ev.block_index} dur={ev.duration_us} us "
                    f"section={ev.section} core={ev.core}\n")

        # Memory
        f.write("\n## Memory\n\n")
        for line in data["memory_lines"]:
            f.write(f"`{line}`\n")

        # Stability
        if data["stability_line"]:
            f.write(f"\n## Stability\n\n`{data['stability_line']}`\n")

    print(f"  wrote {path}")


def main():
    parser = argparse.ArgumentParser(description="B4C.8 serial output parser")
    parser.add_argument("--outdir", default="artifacts/alpha01c/",
                        help="Output directory for CSV/TXT files")
    parser.add_argument("input", nargs="?", type=argparse.FileType("r"),
                        default=sys.stdin,
                        help="Serial output file (default: stdin)")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    lines = args.input.readlines()
    print(f"Parsed {len(lines)} lines")

    data = parse_lines(lines)

    print(f"Build mode: {data['build_mode']}")
    print(f"Voices with Other data: {sorted(data['other_cats'].keys())}")
    print(f"Slow blocks: {sum(len(v) for v in data['slow_blocks'].values())}")
    print(f"Stall events: {len(data['stall_events'])}")
    print(f"Qualify results: {len(data['qualify_results'])}")

    print("\nGenerating artifacts:")
    write_other_breakdown(data, args.outdir)
    write_slow_blocks(data, args.outdir)
    write_stall_events(data, args.outdir)
    write_burst_correlations(data, args.outdir)
    write_cache_layout(data, args.outdir)
    write_memory(data, args.outdir)
    write_transport(data, args.outdir)
    write_lpc_gate(data, args.outdir)
    write_report(data, args.outdir)

    print("\nDone.")


if __name__ == "__main__":
    main()
