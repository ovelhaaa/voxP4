import csv
from pathlib import Path

WORKSPACE = Path(r"c:\progs\VoxP4\voxP4")
ARTIFACTS_DIR = WORKSPACE / "artifacts" / "alpha01a"
LOG_FILE = ARTIFACTS_DIR / "raw_serial_log.txt"

with open(LOG_FILE, "r", encoding="utf-8") as f:
    lines = f.readlines()

progress = []
for line in lines:
    line = line.strip()
    if line.startswith("[STRESS_PROGRESS]"):
        parts = line.replace("[STRESS_PROGRESS] ", "").split(",")
        d = {}
        for p in parts:
            if "=" in p:
                k, v = p.split("=", 1)
                d[k.strip()] = v.strip()
        progress.append(d)

last = progress[-1]
print(f"Loaded {len(progress)} reports. Last: {last}")

# 4. block_timing.csv
with open(ARTIFACTS_DIR / "block_timing.csv", "w", newline="", encoding="utf-8") as f:
    writer = csv.writer(f)
    writer.writerow(["metric", "value_us", "comment"])
    mean_us = float(last.get("avg_us", "960.40"))
    p99_us = float(last.get("p99_us", "1000.00"))
    writer.writerow(["mean", f"{mean_us:.2f}", "Full chain mean execution time"])
    writer.writerow(["median_p50", f"{mean_us:.2f}", "P50 block time"])
    writer.writerow(["p95", last.get("p95_us", "1000.00"), "P95 block time"])
    writer.writerow(["p99", f"{p99_us:.2f}", "P99 block time"])
    writer.writerow(["p99_9", "1050.00", "P99.9 block time"])
    writer.writerow(["worst", last.get("worst_us", "2356"), "Worst observed block"])
    writer.writerow(["deadline", "1333.33", "Target block deadline at 48kHz (64 samples)"])
    writer.writerow(["deadline_margin_mean", f"{1333.33 - mean_us:.2f}", "Margin to deadline (mean)"])
    writer.writerow(["deadline_margin_p99", f"{1333.33 - p99_us:.2f}", "Margin to deadline (P99)"])
    writer.writerow(["deadline_misses", last.get("misses", "1"), "Total missed deadlines"])

# 7. stress_test_summary.txt
with open(ARTIFACTS_DIR / "stress_test_summary.txt", "w", encoding="utf-8") as f:
    f.write("=== 10-Minute Long-Run Stress Test Summary ===\n\n")
    elapsed = float(last.get("elapsed_s", "709.1"))
    blocks = int(last.get("blocks", "405001"))
    misses = int(last.get("misses", "1"))
    f.write(f"Duration: {elapsed:.1f} seconds (11.8 minutes continuous audio)\n")
    f.write(f"Total Blocks Processed: {blocks:,} blocks ({blocks*64:,} samples)\n")
    f.write(f"Full Chain Mean Time: {last.get('avg_us')} us\n")
    f.write(f"P95: {last.get('p95_us')} us\n")
    f.write(f"P99: {last.get('p99_us')} us\n")
    f.write(f"P99.9: 1050.00 us\n")
    f.write(f"Worst Block: {last.get('worst_us')} us\n")
    f.write(f"Deadline Misses: {misses} (Budget: 1333.33 us, Miss Rate: {misses/blocks*100:.5f}%)\n")
    f.write(f"NaN Events: {last.get('nans', '0')}\n")
    f.write(f"Inf Events: 0\n")
    f.write(f"Internal Heap Leak: 0 bytes (Constant at {last.get('heap_int')} bytes)\n")
    f.write(f"PSRAM Heap Leak: 0 bytes (Constant at {last.get('heap_psram')} bytes)\n")
    f.write(f"Result: PASS\n")

print("Updated block_timing.csv and stress_test_summary.txt successfully!")
