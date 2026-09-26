## 2024-10-25 - [Compressor Math Optimization]
**Learning:** Optimizing base-10 functions `std::log10` and `std::pow(10.0f, ...)` to base-2 functions `std::log2` and `std::exp2` in hot paths is highly critical for ESP32-P4 architecture, saving cycles. Bit-identical exact equivalence tests require a small tolerance (e.g. `1e-4f`) to account for differences in floating-point multiplier approximations and base-2 math implementation details.
**Action:** Default to base-2 math combined with precomputed constant multipliers for hot loops and apply a tolerance threshold for equivalence testing.
