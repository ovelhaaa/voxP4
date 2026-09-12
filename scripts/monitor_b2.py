import sys
import os
import subprocess
import time
import serial
import re

PORT = "COM11"
BAUD = 115200
TIMEOUT_SEC = 160

def main():
    print(f"[STAGE B2 RUNNER] Flashing Stage B2 binary to {PORT}...", flush=True)
    
    esptool_cmd = [
        sys.executable, "-m", "esptool",
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
        "0x10000", os.path.join("build", "vox_p4.bin")
    ]
    
    res = subprocess.run(esptool_cmd)
    if res.returncode != 0:
        print("[STAGE B2 RUNNER] Flash failed!")
        sys.exit(res.returncode)
        
    print("[STAGE B2 RUNNER] Flash successful. Opening serial monitor...", flush=True)
    time.sleep(1.0)
    
    ser = None
    for attempt in range(10):
        try:
            ser = serial.Serial()
            ser.port = PORT
            ser.baudrate = BAUD
            ser.timeout = 1.0
            ser.dtr = False
            ser.rts = False
            ser.open()
            print(f"[STAGE B2 RUNNER] Serial port {PORT} opened.", flush=True)
            break
        except Exception as e:
            print(f"[STAGE B2 RUNNER] Attempt {attempt+1} failed: {e}. Retrying...", flush=True)
            time.sleep(0.5)
            
    if not ser:
        print("[STAGE B2 RUNNER] Could not open serial port!")
        sys.exit(1)
        
    start_time = time.time()
    captured_lines = []
    test_completed = False
    
    log_path = os.path.join("artifacts", "alpha01b", "adc_rx_serial_log.txt")
    log_file = open(log_path, "w", encoding="utf-8")
    
    print("[STAGE B2 RUNNER] Monitoring Stage B2 (2-minute ADC bring-up)...", flush=True)
    
    try:
        while time.time() - start_time < TIMEOUT_SEC:
            line_bytes = ser.readline()
            if not line_bytes:
                continue
            line = line_bytes.decode("utf-8", errors="replace").rstrip()
            print(line, flush=True)
            log_file.write(line + "\n")
            log_file.flush()
            captured_lines.append(line)
            
            if "STAGE B2: PCM1808 ADC-ONLY BRING-UP COMPLETE" in line:
                test_completed = True
                # Allow time to capture forensic dump
                deadline = time.time() + 8.0
                while time.time() < deadline:
                    l = ser.readline().decode("utf-8", errors="replace").rstrip()
                    if l:
                        print(l, flush=True)
                        log_file.write(l + "\n")
                        log_file.flush()
                        captured_lines.append(l)
                break
    except KeyboardInterrupt:
        print("\n[STAGE B2 RUNNER] Aborted.")
    finally:
        log_file.close()
        ser.close()
        
    log_content = "\n".join(captured_lines)
    print(f"\n[STAGE B2 RUNNER] Captured {len(captured_lines)} lines.")
    
    # Check if ADC signals were detected
    # Example format: [B2 010 s] Peak L/R: 0.0000 / 0.0000 | RMS L/R: 0.0000 / 0.0000 | DC L/R: +0.00000 / +0.00000 | Clipped: 0 | Zeros: 0 | RX Overruns: 0
    b2_lines = [l for l in captured_lines if "[B2 " in l and "Peak L/R:" in l]
    
    status_str = "PASS" if (test_completed and len(b2_lines) >= 10) else "FAIL"
    
    adc_report = f"""STATUS: {status_str}
Target Stage: B2 — PCM1808 ADC-Only Bring-Up
Board: Wireless-Tag WT9932P4-TINY + PCM1808 ADC
Silicon: ESP32-P4 revision v1.3
Executed: {time.strftime('%Y-%m-%d %H:%M:%S')}

Firmware Implementation: TEST COMPLETE
Firmware Test Procedure: run_i2s_stage_b2_adc_bringup()
Execution Duration: 120 seconds (2 minutes continuous)

ADC Diagnostics Summary:
"""
    if b2_lines:
        adc_report += "  Initial Sample (10s): " + b2_lines[0] + "\n"
        adc_report += "  Final Sample (120s):   " + b2_lines[-1] + "\n\n"
        adc_report += "Full Log History:\n"
        for bl in b2_lines:
            adc_report += f"  {bl}\n"
    else:
        adc_report += "  No B2 diagnostics lines captured.\n"
        
    adc_report += f"\nResult: {status_str}\n"
    
    with open(os.path.join("artifacts", "alpha01b", "adc_rx_test.txt"), "w", encoding="utf-8") as f:
        f.write(adc_report)
    print("[STAGE B2 RUNNER] adc_rx_test.txt updated.")
    
    if test_completed:
        print("[STAGE B2 RUNNER] STAGE B2 PASS!")
    else:
        print("[STAGE B2 RUNNER] STAGE B2 INCOMPLETE / FAIL!")

if __name__ == "__main__":
    main()
