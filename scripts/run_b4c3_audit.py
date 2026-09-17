# Stage B4C.3 Runner and Report Generator
import argparse
import pathlib
import re
import subprocess
import sys
import time
import serial

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'artifacts' / 'alpha01c'
ANSI = re.compile(r'\x1b\[[0-9;]*m')

def flash(port: str):
    print(f'Flashing firmware to {port}...', flush=True)
    export = pathlib.Path.home() / 'esp/esp-idf/export.ps1'
    cmd = f". '{export}'; idf.py -p {port} flash"
    res = subprocess.run(['pwsh', '-Command', cmd], cwd=ROOT)
    if res.returncode != 0:
        print(f'Flashing failed: {res.returncode}', flush=True)
        sys.exit(res.returncode)
    print('Flashing completed successfully!\n', flush=True)

def capture(port: str, timeout: float = 600.0, baud: int = 115200) -> list[str]:
    lines = []
    final_seen = False
    print(f'Listening on {port} at {baud} baud (timeout={timeout}s)...', flush=True)
    with serial.Serial(port, baud, timeout=0.25, dsrdtr=False, rtscts=False) as s:
        s.dtr = False
        s.rts = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = s.readline()
            if not raw:
                continue
            line = ANSI.sub('', raw.decode('utf-8', errors='replace')).rstrip()
            print(line, flush=True)
            lines.append(line)
            if 'HARMONIZER-ONLY CAPACITY QUALIFIED:' in line:
                final_seen = True
            elif final_seen and 'NEXT STEP:' in line:
                deadline = min(deadline, time.monotonic() + 5.0)
    return lines

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM11')
    parser.add_argument('--timeout', type=float, default=600.0)
    parser.add_argument('--skip-flash', action='store_true')
    args = parser.parse_args()

    if not args.skip_flash:
        flash(args.port)

    lines = capture(args.port, args.timeout)

    OUT.mkdir(parents=True, exist_ok=True)
    log_file = OUT / 'b4c3_serial.log'
    log_file.write_text('\n'.join(lines), encoding='utf-8')
    print(f'\nSerial log saved to {log_file}')

    # Find audit output
    start_idx = 0
    for i, line in enumerate(lines):
        if 'STAGE B4C.3: HARMONIZER TAIL-LATENCY & MODELWARP OPTIMIZATION AUDIT' in line:
            start_idx = i
            break
    audit_lines = lines[start_idx:] if start_idx < len(lines) else lines

    report = [
        '# Stage B4C.3 — Harmonizer Tail-Latency & ModelWarp Optimization Report',
        '',
        '**Target Hardware**: Wireless-Tag WT9932P4-TINY (ESP32-P4 @ 360 MHz)',
        f'**Timestamp**: {time.strftime("%Y-%m-%d %H:%M:%S")}',
        '',
        '---',
        '',
        '```text',
    ]
    report.extend(audit_lines)
    report.append('```\n')

    rep_file = OUT / 'b4c3_harmonizer_tail_report.md'
    rep_file.write_text('\n'.join(report), encoding='utf-8')
    print(f'Report saved to {rep_file}')

if __name__ == '__main__':
    main()
