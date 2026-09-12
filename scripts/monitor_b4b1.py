import sys
import os
import subprocess
import time
import serial
import re

PORT = "COM11"
BAUD = 115200
TIMEOUT_SEC = 120  # 50s test + margin

def main():
    print(f"[STAGE B4B.1 RUNNER] Flashing Stage B4B.1 binary to {PORT}...", flush=True)
    
    idf_py = r"C:\Users\devx\.espressif\python_env\idf5.3_py3.14_env\Scripts\python.exe"
    py_bin = idf_py if os.path.exists(idf_py) else sys.executable
    
    esptool_cmd = [
        py_bin, "-m", "esptool",
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
        print("[STAGE B4B.1 RUNNER] Flash failed!")
        sys.exit(res.returncode)
        
    print("[STAGE B4B.1 RUNNER] Flash successful. Opening serial monitor...", flush=True)
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
            print(f"[STAGE B4B.1 RUNNER] Serial port {PORT} opened.", flush=True)
            break
        except Exception as e:
            print(f"[STAGE B4B.1 RUNNER] Attempt {attempt+1} failed: {e}. Retrying...", flush=True)
            time.sleep(0.5)
            
    if not ser:
        print("[STAGE B4B.1 RUNNER] Could not open serial port!")
        sys.exit(1)
        
    start_time = time.time()
    captured_lines = []
    test_completed = False
    
    os.makedirs(os.path.join("artifacts", "alpha01b"), exist_ok=True)
    log_path = os.path.join("artifacts", "alpha01b", "b4b1_serial_log.txt")
    log_file = open(log_path, "w", encoding="utf-8")
    
    print("[STAGE B4B.1 RUNNER] Monitoring Stage B4B.1 Audit...", flush=True)
    
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
            
            if "STAGE B4B.1: HARMONY ACTIVATION AUDIT COMPLETE" in line:
                test_completed = True
                deadline = time.time() + 10.0
                while time.time() < deadline:
                    l_bytes = ser.readline()
                    if not l_bytes:
                        continue
                    l = l_bytes.decode("utf-8", errors="replace").rstrip()
                    print(l, flush=True)
                    log_file.write(l + "\n")
                    log_file.flush()
                    captured_lines.append(l)
                break
    except KeyboardInterrupt:
        print("\n[STAGE B4B.1 RUNNER] Aborted by user.")
    finally:
        log_file.close()
        ser.close()
        
    log_content = "\n".join(captured_lines)
    print(f"\n[STAGE B4B.1 RUNNER] Captured {len(captured_lines)} lines.")

if __name__ == "__main__":
    main()
