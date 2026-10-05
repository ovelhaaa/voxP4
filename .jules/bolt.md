
## 2024-10-05 - Base-2 Math for Audio DSP Loops
**Learning:** In hot audio DSP loops on the ESP32-P4, using base-10 functions (e.g., `std::pow(10.0f, ...)`, `std::log10`) creates a significant performance bottleneck.
**Action:** Always prefer base-2 math functions (e.g., `std::exp2`, `std::log2`) over base-10 functions, combined with precomputed constant multipliers for conversions, to achieve significant performance gains. When doing so, exact-equivalence unit tests will require a small floating-point tolerance (e.g., `1e-4f`).
