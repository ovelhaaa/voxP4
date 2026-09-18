# VoxLink integration export

Machine-consumable artifacts for `ovelhaaa/voxP4-control` (the CYD project).
These are exported from the VoxP4 source of truth and must not be edited by hand.

| File | Description |
|---|---|
| `VoxP4ParamIds.h` | `#define` IDs and `VOXP4_PARAM_COUNT` (45) |
| `voxlink_params.json` | full schema: id, key, type, min, max, default, step, unit, group, flags, smoothing, dsp_binding |
| `voxlink_v1_vectors.h` | golden frame bytes for CRC/framing regression tests |

Regenerate with:

```sh
python tools/export_voxlink_schema.py
```

The normative protocol description is `docs/voxlink_v1.md`; the client-facing
integration contract is `docs/voxlink_cyd_integration.md`.

Copy the three files into the CYD project (or a shared `voxlink/` include
directory) and add a client-side test that decodes `voxlink_v1_vectors.h` and
recomputes the CRC.
