import sys
import os
import subprocess
import time
import serial
import re
import json

PORT = "COM11"
BAUD = 115200
TIMEOUT_SEC = 360 # 5 cases x 20s + 1 soak x 60s + prestaging + margin

def main():
    print(f"[MICROSHIFT RUNNER] Flashing ESP32-P4 on {PORT}...")
    
    python_exe = sys.executable
    # Use IDF python if sys.executable doesn't have esptool
    idf_py = r"C:\Users\devx\.espressif\python_env\idf5.3_py3.14_env\Scripts\python.exe"
    if os.path.exists(idf_py):
        python_exe = idf_py

    esptool_cmd = [
        python_exe, "-m", "esptool",
        "--chip", "esp32p4",
        "-p", PORT,
        "-b", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--force",
        "--flash_mode", "dio",
        "--flash_size", "2MB",
        "--flash_freq", "80m",
        "0x2000", os.path.join("build", "bootloader", "bootloader.bin"),
        "0x8000", os.path.join("build", "partition_table", "partition-table.bin"),
        "0x10000", os.path.join("build", "vox_p4.bin")
    ]
    
    res = subprocess.run(esptool_cmd)
    if res.returncode != 0:
        print("[MICROSHIFT RUNNER] Flash failed!")
        sys.exit(res.returncode)
        
    print(f"[MICROSHIFT RUNNER] Flash successful. Opening serial monitor on {PORT}...")
    time.sleep(1.5)
    
    ser = None
    for attempt in range(10):
        try:
            ser = serial.Serial(PORT, BAUD, timeout=1.0)
            print(f"[MICROSHIFT RUNNER] Serial port {PORT} opened.")
            break
        except Exception as e:
            print(f"[MICROSHIFT RUNNER] Attempt {attempt+1} failed: {e}. Retrying in 0.5s...")
            time.sleep(0.5)
            
    if not ser:
        print("[MICROSHIFT RUNNER] Failed to open serial port after flashing!")
        sys.exit(1)
        
    start_time = time.time()
    captured_lines = []
    matrix_completed = False
    
    print("[MICROSHIFT RUNNER] Monitoring serial output for Microshift qualification matrix...")
    
    try:
        while time.time() - start_time < TIMEOUT_SEC:
            line_bytes = ser.readline()
            if not line_bytes:
                continue
            line = line_bytes.decode("utf-8", errors="replace").rstrip()
            print(line, flush=True)
            captured_lines.append(line)
            
            if "=== END OF MICROSHIFT QUALIFICATION MATRIX ===" in line:
                matrix_completed = True
                # Flush trailing messages
                flush_end = time.time() + 3.0
                while time.time() < flush_end:
                    l = ser.readline().decode("utf-8", errors="replace").rstrip()
                    if l:
                        print(l, flush=True)
                        captured_lines.append(l)
                break
    except KeyboardInterrupt:
        print("\n[MICROSHIFT RUNNER] Aborted by user.")
    finally:
        ser.close()
        
    log_content = "\n".join(captured_lines)
    os.makedirs(os.path.join("artifacts", "microshift_hardware"), exist_ok=True)
    raw_path = os.path.join("artifacts", "microshift_hardware", "microshift_p4_qualification_raw.txt")
    with open(raw_path, "w", encoding="utf-8") as f:
        f.write(log_content)
    print(f"\n[MICROSHIFT RUNNER] Raw log saved to {raw_path}")

    # Parse case statistics
    results = {}
    current_case = None

    for line in captured_lines:
        m_dsp = re.match(r"B4D12_DSP name=(\S+) blocks=(\d+) avg_us=([\d\.]+) p50=([\d\.]+) p90=([\d\.]+) p95=([\d\.]+) p99=([\d\.]+) p999=([\d\.]+) p9999=([\d\.]+) max=([\d\.]+) misses=(\d+)", line)
        if m_dsp:
            name = m_dsp.group(1)
            results.setdefault(name, {})["dsp"] = {
                "blocks": int(m_dsp.group(2)),
                "mean_us": float(m_dsp.group(3)),
                "p50_us": float(m_dsp.group(4)),
                "p90_us": float(m_dsp.group(5)),
                "p95_us": float(m_dsp.group(6)),
                "p99_us": float(m_dsp.group(7)),
                "p999_us": float(m_dsp.group(8)),
                "max_us": float(m_dsp.group(10)),
                "misses": int(m_dsp.group(11))
            }
        
        m_late = re.match(r"B4D12_LATE name=(\S+) .*max_consecutive_late=(\d+)", line)
        if m_late:
            name = m_late.group(1)
            results.setdefault(name, {})["late"] = {
                "max_consecutive_late": int(m_late.group(2))
            }

        m_tr = re.match(r"B4D12_TRANSPORT name=(\S+) rx_overruns=(\d+) tx_underruns=(\d+) dma_errors=(\d+) actual_rx_dma_errors=(\d+) actual_tx_dma_errors=(\d+) read_failures=(\d+) write_failures=(\d+) rx_dropped_frames=(\d+) tx_dropped_frames=(\d+)", line)
        if m_tr:
            name = m_tr.group(1)
            total_tr = sum(int(m_tr.group(i)) for i in range(2, 11))
            results.setdefault(name, {})["transport"] = {
                "total_errors": total_tr,
                "rx_overruns": int(m_tr.group(2)),
                "tx_underruns": int(m_tr.group(3)),
                "dma_errors": int(m_tr.group(4))
            }

        m_stage = re.match(r"B4D12_STAGE name=(\S+) stage=(\S+) blocks=(\d+) avg_cycles=([\d\.]+) max_cycles=(\d+)", line)
        if m_stage:
            name = m_stage.group(1)
            stage = m_stage.group(2)
            results.setdefault(name, {}).setdefault("stages", {})[stage] = {
                "avg_cycles": float(m_stage.group(4)),
                "max_cycles": int(m_stage.group(5))
            }

    analysis_path = os.path.join("artifacts", "microshift_hardware", "microshift_physical_analysis.json")
    with open(analysis_path, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
    print(f"[MICROSHIFT RUNNER] Analysis saved to {analysis_path}")
    
    print("\n==========================================================================")
    print("                ESP32-P4 PHYSICAL QUALIFICATION RESULTS                   ")
    print("==========================================================================")
    print(f"{'Case Name':<22} | {'Mean (us)':<9} | {'p50':<5} | {'p95':<5} | {'p99':<5} | {'p99.9':<6} | {'Max':<5} | {'Miss':<5} | {'Late':<4} | {'Mod Cyc':<8} | {'Pipe Cyc':<9}")
    print("-" * 105)
    for name, data in results.items():
        if not name.startswith("mod_"):
            continue
        dsp = data.get("dsp", {})
        late = data.get("late", {})
        stages = data.get("stages", {})
        mod_cyc = stages.get("Chorus", {}).get("avg_cycles", 0.0)
        pipe_cyc = stages.get("Pipeline", {}).get("avg_cycles", 0.0)
        print(f"{name:<22} | {dsp.get('mean_us', 0.0):<9.2f} | {dsp.get('p50_us', 0.0):<5.0f} | {dsp.get('p95_us', 0.0):<5.0f} | {dsp.get('p99_us', 0.0):<5.0f} | {dsp.get('p999_us', 0.0):<6.0f} | {dsp.get('max_us', 0.0):<5.0f} | {dsp.get('misses', 0):<5} | {late.get('max_consecutive_late', 0):<4} | {mod_cyc:<8.0f} | {pipe_cyc:<9.0f}")
    print("-" * 105)

if __name__ == "__main__":
    main()
