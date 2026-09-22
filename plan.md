1. **Analyze optimization opportunities:** I've identified that `std::log10` and `std::pow(10.0f, ...)` are used in hot paths (`components/vocal_fx/dynamics/compressor.cpp`, `components/vocal_fx/dynamics/gate.cpp`). The DSP instructions specifically mention: "In hot audio DSP loops on the ESP32-P4, prefer base-2 math functions (e.g., `std::exp2`, `std::log2`) over base-10 functions (e.g., `std::pow(10.0f, ...)`, `std::log10`) combined with precomputed constant multipliers for significant performance gains."
2. **Apply optimizations:**
   - In `compressor.cpp`, replace `20 * std::log10(x)` with `std::log2(x) * 6.020599913279624f`.
   - In `compressor.cpp`, replace `std::pow(10.0f, x / 20)` with `std::exp2(x * 0.16609640474436813f)`.
   - In `gate.cpp`, replace `std::pow(10.0f, x / 20)` with `std::exp2(x * 0.16609640474436813f)`.
3. **Adjust tolerance in unit tests:** Update `almost_equal` checks in `tests/test_b4d3_equivalence.cpp` to use a small tolerance like `1e-4f` instead of checking for bit-exactness since base-2 math involves an approximation.
4. **Complete pre-commit checks:** Run `pre_commit_instructions` to ensure testing, verification, review, and reflection steps are complete.
5. **Submit changes:** Use `submit` to commit the performance improvements with appropriate documentation.
