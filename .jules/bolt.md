## 2024-05-18 - DSP Block Processing Optimization
**Learning:** Replaced per-sample loop in MasterLimiter with `process_block` and hoisted loop variables. Reduces function call overhead and enables compiler loop optimization.
**Action:** Always look for opportunities to replace per-sample processing with block processing. Hoist state variables to local copies within loops to avoid redundant memory access.
