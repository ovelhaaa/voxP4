#!/usr/bin/env python3
"""Export the VoxLink parameter registry for the CYD project.

Parses the macro table in components/voxlink/src/voxlink_registry.cpp (the
single source of truth) and emits CYD-consumable artifacts:

    integration/voxlink_params.json    full machine-readable schema
    integration/VoxP4ParamIds.h        C/C++ ID and count defines

Run:
    python tools/export_voxlink_schema.py
"""
from __future__ import annotations

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(ROOT, "components", "voxlink", "src",
                        "voxlink_registry.cpp")
OUT_DIR = os.path.join(ROOT, "integration")

FLAG_BITS = {
    "kParamPersistent": 1,
    "kParamRealtimeSafe": 2,
    "kParamReadOnly": 4,
    "kParamTelemetry": 8,
    "kParamRequiresReinit": 16,
    "kParamSmoothed": 32,
    "kParamDiscrete": 64,
    "kPersistRt": 1 | 2,
    "kPersistRtSmoothed": 1 | 2 | 32,
    "kPersistRtDiscrete": 1 | 2 | 64,
}
TAGS = {"bool": 1, "int": 2, "float": 3, "enum": 4}

# (macro, field names after the fixed first fields)
MACROS = {
    "PBOOL": ("bool", ["def", "flags", "group", "binding"]),
    "PINT": ("int", ["unit", "lo", "hi", "def", "flags", "group", "binding"]),
    "PENUM": ("enum", ["lo", "hi", "def", "group", "binding"]),
    "PFLOAT": ("float", ["unit", "lo", "hi", "def", "step", "flags", "smooth",
                         "group", "binding"]),
}


def num(token: str) -> float:
    token = token.strip()
    if token[-1:] in ("f", "F"):
        token = token[:-1]
    return float(token)


def unq(token: str) -> str:
    """Strip the surrounding C string quotes captured by the field regex."""
    token = token.strip()
    if len(token) >= 2 and token[0] == '"' and token[-1] == '"':
        token = token[1:-1]
    return token


def parse():
    text = open(REGISTRY, encoding="utf-8").read()
    # Drop macro definitions so only invocations remain.
    text = re.sub(r"(?m)^\s*#\s*(define|undef)\s+.*$", "", text)
    # Strip line continuations.
    text = text.replace("\\\n", " ")
    params = []
    for macro, (ptype, tail_fields) in MACROS.items():
        pattern = re.compile(
            re.escape(macro) + r"\(\s*" +
            r",\s*".join([r"(0x[0-9A-Fa-f]+)", r'"([^"]*)"', r'"([^"]*)"'] +
                         [r"([^,)]+)" for _ in tail_fields]) + r"\s*\)",
            re.S)
        for m in pattern.finditer(text):
            groups = m.groups()
            pid = int(groups[0], 16)
            key = groups[1]
            display = groups[2]
            tail = groups[3:]
            rec = {"id": pid, "key": key, "display": display, "type": ptype}
            cont = dict(zip(tail_fields, [g.strip() for g in tail]))
            if ptype == "bool":
                rec.update(min=0.0, max=1.0, default=num(cont["def"]),
                           step=1.0, unit="", flags=cont["flags"],
                           smoothing_ms=0.0, group=cont["group"],
                           dsp_binding=cont["binding"])
            elif ptype == "int":
                rec.update(min=num(cont["lo"]), max=num(cont["hi"]),
                           default=num(cont["def"]), step=1.0,
                           unit=cont["unit"], flags=cont["flags"],
                           smoothing_ms=0.0, group=cont["group"],
                           dsp_binding=cont["binding"])
            elif ptype == "enum":
                rec.update(min=num(cont["lo"]), max=num(cont["hi"]),
                           default=num(cont["def"]), step=1.0, unit="",
                           flags="kPersistRtDiscrete", smoothing_ms=0.0,
                           group=cont["group"], dsp_binding=cont["binding"])
            else:
                rec.update(min=num(cont["lo"]), max=num(cont["hi"]),
                           default=num(cont["def"]), step=num(cont["step"]),
                           unit=cont["unit"], flags=cont["flags"],
                           smoothing_ms=num(cont["smooth"]),
                           group=cont["group"], dsp_binding=cont["binding"])
            # The field regex captures the surrounding C quotes; strip them so
            # the JSON carries clean strings (group/unit/dsp_binding).
            for field in ("group", "unit", "dsp_binding"):
                if field in rec:
                    rec[field] = unq(rec[field])
            rec["tag"] = TAGS[ptype]
            rec["flags_value"] = FLAG_BITS[rec["flags"]]
            params.append(rec)
    params.sort(key=lambda r: r["id"])
    return params


def write_json(params):
    path = os.path.join(OUT_DIR, "voxlink_params.json")
    doc = {
        "voxlink_version": "1.0",
        "parameter_count": len(params),
        "value_tags": {"bool": 1, "int32": 2, "float32": 3, "enum16": 4},
        "parameters": params,
    }
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2, sort_keys=False)
        fh.write("\n")
    return path


def write_header(params):
    path = os.path.join(OUT_DIR, "VoxP4ParamIds.h")
    lines = [
        "#pragma once",
        "// GENERATED by tools/export_voxlink_schema.py from",
        "// components/voxlink/src/voxlink_registry.cpp. Do not edit by hand.",
        "",
        "#define VOXP4_VOXLINK_VERSION 0x10u",
        "#define VOXP4_PARAM_COUNT %du" % len(params),
        "",
    ]
    for rec in params:
        name = "VOXP4_PARAM_" + re.sub(r"[^A-Za-z0-9]+", "_",
                                       rec["key"]).upper()
        lines.append("#define %-40s 0x%04Xu  /* %s */" %
                     (name, rec["id"], rec["type"]))
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    return path


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    params = parse()
    ids = [p["id"] for p in params]
    keys = [p["key"] for p in params]
    if len(set(ids)) != len(ids):
        print("ERROR: duplicate ids", file=sys.stderr)
        return 1
    if len(set(keys)) != len(keys):
        print("ERROR: duplicate keys", file=sys.stderr)
        return 1
    j = write_json(params)
    h = write_header(params)
    print("exported %d parameters" % len(params))
    print("  " + os.path.relpath(j, ROOT))
    print("  " + os.path.relpath(h, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
