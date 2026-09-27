## 2024-05-24 - Base-2 Math Optimization in ESP32-P4
**Learning:** In hot audio DSP loops on the ESP32-P4, using base-10 math functions (`std::pow`, `std::log10`) introduces performance bottlenecks compared to base-2 equivalents.
**Action:** Prefer base-2 math functions (`std::exp2`, `std::log2`) over base-10 functions combined with precomputed constant multipliers for significant performance gains. When doing so, exact-equivalence unit tests will require a small floating-point tolerance (e.g., `1e-4f`).
