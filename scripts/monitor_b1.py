import sys
import serial
import time
import re
import os

PORT = "COM11"
BAUD = 115200
TIMEOUT_SEC = 340

def main():
    print(f"[MONITOR] Opening {PORT} with dtr=False, rts=False...", flush=True)
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1.0
    ser.dtr = False
    ser.rts = False
    ser.open()
    
    # If the board was waiting or needs a clean restart to capture from boot:
    print("[MONITOR] Sending reset sequence to ESP32-P4...", flush=True)
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.2)
    ser.reset_input_buffer()
    
    print("[MONITOR] Reset released. Capturing boot and test stream...", flush=True)
    start_time = time.time()
    captured_lines = []
    test_completed = False
    
    log_file = open(os.path.join("artifacts", "alpha01b", "dac_tx_serial_log.txt"), "w", encoding="utf-8")
    
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
            
            if "STAGE B1: PCM5102 DAC-ONLY BRING-UP COMPLETE" in line:
                test_completed = True
                # Read a few more lines to capture the summary block
                deadline = time.time() + 3.0
                while time.time() < deadline:
                    l = ser.readline().decode("utf-8", errors="replace").rstrip()
                    if l:
                        print(l, flush=True)
                        log_file.write(l + "\n")
                        log_file.flush()
                        captured_lines.append(l)
                break
    except KeyboardInterrupt:
        print("\n[MONITOR] Aborted.")
    finally:
        log_file.close()
        ser.close()
        
    print(f"\n[MONITOR] Done. Captured {len(captured_lines)} lines.")

if __name__ == "__main__":
    main()
