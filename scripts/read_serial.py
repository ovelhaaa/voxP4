import serial, time, sys
s = serial.Serial('COM11', 115200, timeout=1)
time.sleep(2)
data = b''
start = time.time()
duration = int(sys.argv[1]) if len(sys.argv) > 1 else 30
while time.time() - start < duration:
    chunk = s.read(8192)
    if chunk:
        data += chunk
s.close()
text = data.decode('utf-8', errors='replace')
# Print non-B4C7_BLOCK lines (summary, diagnostics)
b4c7_count = 0
b4c8_count = 0
for line in text.split('\n'):
    stripped = line.strip()
    if not stripped:
        continue
    if stripped.startswith('B4C7_BLOCK'):
        b4c7_count += 1
    else:
        if stripped.startswith('B4C8_'):
            b4c8_count += 1
        print(stripped)
print(f"\n--- B4C7_BLOCK lines: {b4c7_count}, B4C8_* lines: {b4c8_count} ---")
