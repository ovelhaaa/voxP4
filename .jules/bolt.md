## 2024-05-24 - Base-2 Math Optimization in Hot Paths
**Learning:** Using `std::log2` and `std::exp2` with precomputed constants is significantly faster than base-10 equivalents (`std::log10`, `std::pow(10.0f, ...)`) in this codebase's hot paths (e.g. `Compressor::process` and `process_block`).
**Action:** Always prefer base-2 math functions in DSP hot loops for performance gains, and ensure any exact-equivalence tests are updated with a small floating-point tolerance (`1e-4f`) to accommodate the slight precision differences.
