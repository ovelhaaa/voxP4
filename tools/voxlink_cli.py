#!/usr/bin/env python3
"""VoxLink v1 reference client for the VoxP4 control plane.

This tool is the protocol reference used to debug whether a failure is in the
P4, the protocol, or the CYD controller. It implements the wire format,
CRC-16/CCITT-FALSE, framing, and typed parameter values exactly as documented
in docs/voxlink_v1.md.

Examples:
    python tools/voxlink_cli.py --port COM7 hello
    python tools/voxlink_cli.py --port COM7 caps
    python tools/voxlink_cli.py --port COM7 state
    python tools/voxlink_cli.py --port COM7 get 0x0101
    python tools/voxlink_cli.py --port COM7 set harmony.interval 7
    python tools/voxlink_cli.py --port COM7 monitor
    python tools/voxlink_cli.py --port COM7 stress --rate 100 --seconds 10
    python tools/voxlink_cli.py selftest
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass, field

# --- Protocol constants -----------------------------------------------------
SOF0 = 0xA5
SOF1 = 0x5A
VERSION = 0x10
MAJOR = 1
HEADER_SIZE = 8
MAX_PAYLOAD = 512
CRC_SIZE = 2

MSG = {
    "HELLO": 0x01,
    "HELLO_ACK": 0x02,
    "CAPS_REQUEST": 0x03,
    "CAPS_BEGIN": 0x04,
    "CAPS_PARAM": 0x05,
    "CAPS_END": 0x06,
    "GET_STATE": 0x07,
    "STATE_BEGIN": 0x08,
    "STATE_PARAM": 0x09,
    "STATE_END": 0x0A,
    "GET_PARAM": 0x0B,
    "PARAM_VALUE": 0x0C,
    "SET_PARAM": 0x0D,
    "PARAM_CHANGED": 0x0E,
    "ACTION": 0x0F,
    "ACK": 0x10,
    "NACK": 0x11,
    "ERROR": 0x12,
    "HEARTBEAT": 0x60,
    "METER_FRAME": 0x61,
    "PITCH_FRAME": 0x62,
    "DSP_STATUS": 0x63,
}
MSG_NAME = {v: k for k, v in MSG.items()}

CODE = {
    0: "OK", 1: "UNKNOWN_MESSAGE", 2: "UNSUPPORTED_VERSION", 3: "BAD_LENGTH",
    4: "BAD_TYPE", 5: "UNKNOWN_PARAM", 6: "READ_ONLY", 7: "OUT_OF_RANGE",
    8: "INVALID_ENUM", 9: "QUEUE_FULL", 10: "BUSY", 11: "NOT_SUPPORTED",
    12: "INTERNAL_ERROR", 13: "BAD_CRC", 14: "MALFORMED",
}

TAG_BOOL, TAG_INT32, TAG_FLOAT32, TAG_ENUM16 = 1, 2, 3, 4
TAG_SIZE = {TAG_BOOL: 1, TAG_INT32: 4, TAG_FLOAT32: 4, TAG_ENUM16: 2}

GROUP = {0: "harmony", 1: "dynamics", 2: "delay", 3: "reverb", 4: "output"}

# key -> (id, tag). Mirrors the authoritative registry; the P4 remains the
# authority over existence and numerical semantics via CAPS.
PARAMS = {
    "harmony.enable": (0x0100, TAG_BOOL),
    "harmony.interval": (0x0101, TAG_INT32),
    "harmony.level": (0x0102, TAG_FLOAT32),
    "harmony.mode": (0x0103, TAG_ENUM16),
    "harmony.key": (0x0104, TAG_ENUM16),
    "harmony.scale": (0x0105, TAG_ENUM16),
    "harmony.voice1.pan": (0x0107, TAG_FLOAT32),
    "harmony.voice1.degree": (0x0108, TAG_INT32),
    "harmony.voice1.smoothing_ms": (0x0109, TAG_FLOAT32),
    "harmony.formant.enable": (0x010A, TAG_BOOL),
    "harmony.formant.amount": (0x010B, TAG_FLOAT32),
    "harmony.attack_ms": (0x010C, TAG_FLOAT32),
    "harmony.release_ms": (0x010D, TAG_FLOAT32),
    "harmony.limiter.enable": (0x010E, TAG_BOOL),
    "harmony.limiter.threshold_db": (0x010F, TAG_FLOAT32),
    "harmony.dry_alignment.enable": (0x0110, TAG_BOOL),
    "harmony.dry_alignment.ms": (0x0111, TAG_FLOAT32),
    "compressor.enable": (0x0200, TAG_BOOL),
    "compressor.threshold_db": (0x0201, TAG_FLOAT32),
    "compressor.ratio": (0x0202, TAG_FLOAT32),
    "compressor.attack_ms": (0x0203, TAG_FLOAT32),
    "compressor.release_ms": (0x0204, TAG_FLOAT32),
    "compressor.makeup_db": (0x0205, TAG_FLOAT32),
    "compressor.knee_db": (0x0206, TAG_FLOAT32),
    "gate.enable": (0x0207, TAG_BOOL),
    "gate.threshold_db": (0x0208, TAG_FLOAT32),
    "gate.attack_ms": (0x0209, TAG_FLOAT32),
    "gate.hold_ms": (0x020A, TAG_FLOAT32),
    "gate.release_ms": (0x020B, TAG_FLOAT32),
    "gate.range_db": (0x020C, TAG_FLOAT32),
    "delay.enable": (0x0300, TAG_BOOL),
    "delay.left_ms": (0x0301, TAG_FLOAT32),
    "delay.right_ms": (0x0302, TAG_FLOAT32),
    "delay.feedback": (0x0303, TAG_FLOAT32),
    "delay.wet": (0x0304, TAG_FLOAT32),
    "delay.dry": (0x0305, TAG_FLOAT32),
    "delay.feedback_lowpass_hz": (0x0306, TAG_FLOAT32),
    "reverb.enable": (0x0400, TAG_BOOL),
    "reverb.wet": (0x0401, TAG_FLOAT32),
    "reverb.decay_s": (0x0402, TAG_FLOAT32),
    "reverb.damping": (0x0403, TAG_FLOAT32),
    "limiter.ceiling": (0x0500, TAG_FLOAT32),
    "output.mute_dry": (0x0501, TAG_BOOL),
    "output.spatial_routing": (0x0502, TAG_ENUM16),
    "output.spatial_source": (0x0503, TAG_ENUM16),
}
TAG_NAME = {TAG_BOOL: "bool", TAG_INT32: "int", TAG_FLOAT32: "float",
            TAG_ENUM16: "enum"}


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(msg_type: int, seq: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = bytes([SOF0, SOF1, VERSION, msg_type, flags, seq,
                    len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    body = header[2:] + payload
    crc = crc16(body)
    return header + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


@dataclass
class Frame:
    version: int
    msg_type: int
    flags: int
    seq: int
    payload: bytes


class Parser:
    """Streaming parser with a callback for each valid frame."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.crc_errors = 0
        self.resyncs = 0
        self.frames_valid = 0

    def feed(self, data: bytes):
        self.buf.extend(data)
        frames = []
        while True:
            frame = self._try_parse()
            if frame is None:
                break
            frames.append(frame)
        return frames

    def _try_parse(self):
        buf = self.buf
        while len(buf) >= 2 and not (buf[0] == SOF0 and buf[1] == SOF1):
            del buf[0]
            self.resyncs += 1
        if len(buf) < HEADER_SIZE + CRC_SIZE:
            return None
        if (buf[2] >> 4) != MAJOR:
            del buf[0:2]
            self.resyncs += 1
            return None
        length = buf[6] | (buf[7] << 8)
        if length > MAX_PAYLOAD:
            del buf[0:2]
            self.resyncs += 1
            return None
        total = HEADER_SIZE + length + CRC_SIZE
        if len(buf) < total:
            return None
        expected = buf[total - 2] | (buf[total - 1] << 8)
        actual = crc16(bytes(buf[2:HEADER_SIZE + length]))
        if expected != actual:
            self.crc_errors += 1
            del buf[0:total]
            self.resyncs += 1
            return None
        frame = Frame(buf[2], buf[3], buf[4], buf[5], bytes(buf[HEADER_SIZE:HEADER_SIZE + length]))
        del buf[0:total]
        self.frames_valid += 1
        return frame


def decode_value(tag: int, data: bytes):
    if tag == TAG_BOOL:
        return data[0] != 0, 1
    if tag == TAG_INT32:
        return struct.unpack_from("<i", data)[0], 4
    if tag == TAG_FLOAT32:
        return struct.unpack_from("<f", data)[0], 4
    if tag == TAG_ENUM16:
        return struct.unpack_from("<H", data)[0], 2
    raise ValueError("bad tag %d" % tag)


def encode_value(tag: int, value: float) -> bytes:
    if tag == TAG_BOOL:
        return bytes([1 if value >= 0.5 else 0])
    if tag == TAG_INT32:
        return struct.pack("<i", int(round(value)))
    if tag == TAG_FLOAT32:
        return struct.pack("<f", float(value))
    if tag == TAG_ENUM16:
        return struct.pack("<H", int(round(value)))
    raise ValueError("bad tag %d" % tag)


# --- Transport --------------------------------------------------------------
class Transport:
    def __init__(self, port: str | None, baud: int, timeout: float = 1.0):
        self.parser = Parser()
        self.seq = 0
        self.ser = None
        if port is not None:
            try:
                import serial  # type: ignore
            except ImportError as exc:
                raise SystemExit("pyserial is required for serial commands: pip install pyserial") from exc
            self.ser = serial.Serial(port, baud, timeout=timeout)

    def next_seq(self) -> int:
        self.seq = (self.seq + 1) & 0xFF
        return self.seq

    def send(self, msg_type: int, payload: bytes = b"", seq: int | None = None) -> int:
        s = self.next_seq() if seq is None else seq
        self.ser.write(build_frame(msg_type, s, payload))
        return s

    def read_frames(self, timeout: float = 0.5):
        if self.ser is None:
            return []
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                frames = self.parser.feed(chunk)
                if frames:
                    return frames
        return []

    def request(self, msg_type: int, payload: bytes = b"", timeout: float = 1.0):
        seq = self.send(msg_type, payload)
        deadline = time.time() + timeout
        out = []
        while time.time() < deadline:
            for frame in self.read_frames(timeout=0.2):
                out.append(frame)
                if frame.seq == seq and frame.msg_type in (MSG["ACK"], MSG["NACK"], MSG["ERROR"]):
                    return out
                if frame.msg_type in (MSG["CAPS_END"], MSG["STATE_END"]):
                    return out
        return out


# --- Commands ---------------------------------------------------------------
def cmd_hello(tx: Transport, _args) -> int:
    payload = bytes([VERSION, 0x01, 0x00])
    frames = tx.request(MSG["HELLO"], payload)
    for f in frames:
        if f.msg_type == MSG["HELLO_ACK"]:
            p = f.payload
            caps = struct.unpack_from("<I", p, 1)[0]
            sr = struct.unpack_from("<I", p, 5)[0]
            block = struct.unpack_from("<H", p, 9)[0]
            voices = p[11]
            revision = struct.unpack_from("<I", p, 12)[0]
            print(f"HELLO_ACK caps=0x{caps:08X} sample_rate={sr} block={block} "
                  f"harmony_voices={voices} revision={revision}")
            return 0
        if f.msg_type in (MSG["NACK"], MSG["ERROR"]):
            print_nack(f)
            return 1
    print("no HELLO_ACK")
    return 1


def cmd_caps(tx: Transport, _args) -> int:
    frames = tx.request(MSG["CAPS_REQUEST"])
    count = 0
    for f in frames:
        if f.msg_type == MSG["CAPS_PARAM"]:
            p = f.payload
            pid = struct.unpack_from("<H", p, 0)[0]
            tag = p[2]
            flags = struct.unpack_from("<I", p, 3)[0]
            lo, hi, dflt, step, cur = struct.unpack_from("<fffff", p, 7)
            group = GROUP.get(p[27], "?")
            print(f"0x{pid:04X} {group:8s} {TAG_NAME.get(tag, '?'):5s} "
                  f"[{lo:g},{hi:g}] default={dflt:g} step={step:g} "
                  f"current={cur:g} flags=0x{flags:04X}")
            count += 1
    print(f"# {count} parameters")
    return 0 if count else 1


def cmd_state(tx: Transport, _args) -> int:
    frames = tx.request(MSG["GET_STATE"])
    revision = None
    count = 0
    for f in frames:
        if f.msg_type == MSG["STATE_BEGIN"]:
            revision = struct.unpack_from("<I", f.payload, 0)[0]
        elif f.msg_type == MSG["STATE_PARAM"]:
            pid = struct.unpack_from("<H", f.payload, 0)[0]
            tag = f.payload[2]
            value, _ = decode_value(tag, f.payload[3:])
            print(f"0x{pid:04X} = {value}")
            count += 1
    print(f"# revision={revision} parameters={count}")
    return 0 if revision is not None else 1


def resolve_param(name: str):
    if name in PARAMS:
        return PARAMS[name]
    try:
        pid = int(name, 0)
    except ValueError:
        raise SystemExit(f"unknown parameter '{name}'")
    for _key, (known_id, tag) in PARAMS.items():
        if known_id == pid:
            return known_id, tag
    raise SystemExit(f"parameter 0x{pid:04X} not in client table; use --type")


def cmd_get(tx: Transport, args) -> int:
    pid, tag = resolve_param(args.param)
    frames = tx.request(MSG["GET_PARAM"], struct.pack("<H", pid))
    for f in frames:
        if f.msg_type == MSG["PARAM_VALUE"]:
            rpid = struct.unpack_from("<H", f.payload, 0)[0]
            revision = struct.unpack_from("<I", f.payload, 2)[0]
            rtag = f.payload[6]
            value, _ = decode_value(rtag, f.payload[7:])
            print(f"0x{rpid:04X} = {value} (revision {revision})")
            return 0
        if f.msg_type in (MSG["NACK"], MSG["ERROR"]):
            print_nack(f)
            return 1
    print("no PARAM_VALUE")
    return 1


def cmd_set(tx: Transport, args) -> int:
    pid, tag = resolve_param(args.param)
    if args.type:
        tag = {"bool": TAG_BOOL, "int": TAG_INT32, "float": TAG_FLOAT32,
               "enum": TAG_ENUM16}[args.type]
    value = 1.0 if args.value.lower() in ("on", "true") else \
        0.0 if args.value.lower() in ("off", "false") else float(args.value)
    payload = struct.pack("<H", pid) + bytes([tag]) + encode_value(tag, value)
    frames = tx.request(MSG["SET_PARAM"], payload)
    for f in frames:
        if f.msg_type == MSG["PARAM_CHANGED"]:
            rpid = struct.unpack_from("<H", f.payload, 0)[0]
            revision = struct.unpack_from("<I", f.payload, 2)[0]
            rtag = f.payload[7]
            rvalue, _ = decode_value(rtag, f.payload[8:])
            print(f"0x{rpid:04X} = {rvalue} confirmed (revision {revision})")
            return 0
        if f.msg_type in (MSG["NACK"], MSG["ERROR"]):
            print_nack(f)
            return 1
    print("no PARAM_CHANGED")
    return 1


def cmd_monitor(tx: Transport, args) -> int:
    print("# monitoring (Ctrl-C to stop)")
    tx.send(MSG["HELLO"], bytes([VERSION, 0x01, 0x00]))
    end = time.time() + args.seconds if args.seconds else None
    try:
        while end is None or time.time() < end:
            for f in tx.read_frames(timeout=0.5):
                print(f"[{MSG_NAME.get(f.msg_type, hex(f.msg_type))}] "
                      f"seq={f.seq} len={len(f.payload)}")
    except KeyboardInterrupt:
        pass
    return 0


def cmd_stress(tx: Transport, args) -> int:
    tx.send(MSG["HELLO"], bytes([VERSION, 0x01, 0x00]))
    pid, tag = PARAMS["harmony.interval"]
    sent = 0
    confirmed = 0
    start = time.time()
    period = 1.0 / args.rate if args.rate > 0 else 0.0
    while time.time() - start < args.seconds:
        value = 4 + (sent % 8)
        payload = struct.pack("<H", pid) + bytes([tag]) + encode_value(tag, value)
        seq = tx.send(MSG["SET_PARAM"], payload)
        sent += 1
        for f in tx.read_frames(timeout=0.0):
            if f.seq == seq and f.msg_type in (MSG["ACK"], MSG["PARAM_CHANGED"]):
                confirmed += 1
        if period:
            time.sleep(period)
    elapsed = time.time() - start
    print(f"# sent={sent} confirmed={confirmed} elapsed={elapsed:.2f}s "
          f"rate={sent / max(elapsed, 1e-9):.1f}/s crc_errors={tx.parser.crc_errors}")
    return 0


def cmd_selftest(_tx, _args) -> int:
    # Golden vectors from tests/data/voxlink_v1_vectors.h.
    ok = crc16(b"123456789") == 0x29B1
    hello = bytes([0xA5, 0x5A, 0x10, 0x01, 0x00, 0x2A, 0x03, 0x00,
                   0x10, 0x01, 0x00, 0xD5, 0x21])
    ok = ok and build_frame(MSG["HELLO"], 0x2A, bytes([0x10, 0x01, 0x00])) == hello
    set_frame = bytes([0xA5, 0x5A, 0x10, 0x0D, 0x00, 0x01, 0x07, 0x00,
                       0x01, 0x01, 0x02, 0x07, 0x00, 0x00, 0x00, 0x30, 0x1D])
    payload = struct.pack("<H", 0x0101) + bytes([TAG_INT32]) + struct.pack("<i", 7)
    ok = ok and build_frame(MSG["SET_PARAM"], 1, payload) == set_frame
    parser = Parser()
    frames = parser.feed(hello + set_frame)
    ok = ok and [f.msg_type for f in frames] == [MSG["HELLO"], MSG["SET_PARAM"]]
    print("voxlink_cli selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def print_nack(frame: Frame) -> None:
    if len(frame.payload) >= 4:
        ref_type = frame.payload[0]
        ref_seq = frame.payload[1]
        code = struct.unpack_from("<H", frame.payload, 2)[0]
        print(f"{MSG_NAME.get(frame.msg_type, '?')}: ref=0x{ref_type:02X} "
              f"seq={ref_seq} code={code} {CODE.get(code, '?')}")
    else:
        print(f"{MSG_NAME.get(frame.msg_type, '?')}: malformed")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="VoxLink v1 reference client")
    ap.add_argument("--port", help="serial port (e.g. COM7 or /dev/ttyUSB0)")
    ap.add_argument("--baud", type=int, default=921600)
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("hello")
    sub.add_parser("caps")
    sub.add_parser("state")
    p_get = sub.add_parser("get")
    p_get.add_argument("param")
    p_set = sub.add_parser("set")
    p_set.add_argument("param")
    p_set.add_argument("value")
    p_set.add_argument("--type", choices=["bool", "int", "float", "enum"])
    p_mon = sub.add_parser("monitor")
    p_mon.add_argument("--seconds", type=float, default=0.0)
    p_stress = sub.add_parser("stress")
    p_stress.add_argument("--rate", type=float, default=100.0)
    p_stress.add_argument("--seconds", type=float, default=10.0)
    sub.add_parser("selftest")

    args = ap.parse_args(argv)
    if args.command == "selftest":
        return cmd_selftest(None, args)

    tx = Transport(args.port, args.baud)
    if tx.ser is None:
        raise SystemExit("--port is required for serial commands")
    handler = {
        "hello": cmd_hello, "caps": cmd_caps, "state": cmd_state,
        "get": cmd_get, "set": cmd_set, "monitor": cmd_monitor,
        "stress": cmd_stress,
    }[args.command]
    return handler(tx, args)


if __name__ == "__main__":
    sys.exit(main())
