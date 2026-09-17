import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else 'COM11'
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 560
out = sys.argv[3] if len(sys.argv) > 3 else 'artifacts/alpha01d/b4d5_raw.txt'

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 1
s.dtr = False
s.rts = False
s.open()
s.reset_input_buffer()
data = bytearray()
start = time.time()
with open(out, 'wb') as fh:
    while time.time() - start < duration:
        chunk = s.read(16384)
        if chunk:
            fh.write(chunk)
            fh.flush()
            data += chunk
s.close()
text = data.decode('utf-8', errors='replace')
print(f"captured bytes={len(data)} lines={len(text.splitlines())} -> {out}")
print("END_SEEN" if 'B4D1_END' in text else "NO_END")
