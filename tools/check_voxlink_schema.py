#!/usr/bin/env python3
"""Semantic validation of the generated VoxLink schema artifacts.

Checks that integration/voxlink_params.json is parseable, that its count and
entries are coherent with integration/VoxP4ParamIds.h, and that no string field
leaked C quotes. The registry itself is validated by the C++ host tests; this
script guards the generated artifacts.

Run:
    python tools/check_voxlink_schema.py
"""
from __future__ import annotations

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSON_PATH = os.path.join(ROOT, "integration", "voxlink_params.json")
HEADER_PATH = os.path.join(ROOT, "integration", "VoxP4ParamIds.h")

JSON_TYPE_TO_HEADER = {"bool": "bool", "int": "int", "float": "float",
                       "enum": "enum"}


def fail(message: str) -> int:
    print("ERROR: " + message, file=sys.stderr)
    return 1


def mangle(key: str) -> str:
    return "VOXP4_PARAM_" + re.sub(r"[^A-Za-z0-9]+", "_", key).upper()


def main() -> int:
    with open(JSON_PATH, encoding="utf-8") as fh:
        doc = json.load(fh)

    params = doc["parameters"]
    if doc["parameter_count"] != len(params):
        return fail("parameter_count %s != len(parameters) %d" %
                    (doc["parameter_count"], len(params)))

    ids = [p["id"] for p in params]
    keys = [p["key"] for p in params]
    if len(set(ids)) != len(ids):
        return fail("duplicate parameter ids")
    if len(set(keys)) != len(keys):
        return fail("duplicate parameter keys")

    for p in params:
        for field in ("group", "unit", "dsp_binding"):
            value = p.get(field, "")
            if '"' in value:
                return fail("embedded quote in %s.%s: %r" %
                            (p["key"], field, value))
        if p["type"] not in JSON_TYPE_TO_HEADER:
            return fail("unknown type %r for %s" % (p["type"], p["key"]))
        if p["min"] > p["max"]:
            return fail("min > max for %s" % p["key"])
        if not (p["min"] <= p["default"] <= p["max"]):
            return fail("default out of range for %s" % p["key"])

    # Cross-check against the generated header.
    text = open(HEADER_PATH, encoding="utf-8").read()
    declared = re.search(r"#define\s+VOXP4_PARAM_COUNT\s+(\d+)u", text)
    if not declared:
        return fail("VOXP4_PARAM_COUNT not found in header")
    if int(declared.group(1)) != len(params):
        return fail("header count %s != json count %d" %
                    (declared.group(1), len(params)))

    defines = {}
    for line in text.splitlines():
        m = re.match(r"#define\s+(VOXP4_PARAM_[A-Z0-9_]+)\s+0x([0-9A-Fa-f]+)u"
                     r"\s+/\*\s*([a-z]+)\s*\*/", line)
        if m:
            defines[m.group(1)] = (int(m.group(2), 16), m.group(3))

    for p in params:
        name = mangle(p["key"])
        if name not in defines:
            return fail("missing header define %s for %s" % (name, p["key"]))
        hid, htype = defines[name]
        if hid != p["id"]:
            return fail("header id 0x%04X != json 0x%04X for %s" %
                        (hid, p["id"], p["key"]))
        if htype != JSON_TYPE_TO_HEADER[p["type"]]:
            return fail("header type %s != json type %s for %s" %
                        (htype, p["type"], p["key"]))

    print("voxlink schema OK: %d parameters" % len(params))
    return 0


if __name__ == "__main__":
    sys.exit(main())
