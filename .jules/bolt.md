## 2024-06-25 - Exact Equivalence Tolerances for Base-2 Math Substitution
**Learning:** Substituting base-10 functions (`std::pow(10.0f, ...)`, `std::log10`) with base-2 equivalents (`std::exp2`, `std::log2`) using precomputed multipliers provides significant performance gains in DSP hot loops on ESP32-P4 but slightly breaks exact bitwise equivalence in testing.
**Action:** When making this optimization, update `bits_equal` or similar equivalence test assertions to use a small tolerance (e.g. `1e-4f`) to accommodate the minimal floating-point variance introduced by base conversions.
