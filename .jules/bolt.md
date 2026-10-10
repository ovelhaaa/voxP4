## 2024-10-10 - Fast base-2 math for dynamics
**Learning:** ESP32-P4 and generic x86 run `std::log2` and `std::exp2` significantly faster than `std::log10` and `std::pow(10.0f, ...)`. When combined with precomputed constant multipliers for DB conversion, this gives ~2x speedup on pure math operations. Exact equivalence tests require a small floating point tolerance (e.g., `1e-4f`) to accommodate these optimizations.
**Action:** Use base-2 math combined with precomputed constants for dB conversions in hot audio DSP paths (like compressor or limiter).
