## 2024-06-13 - Base-2 Math in ESP32-P4 Audio DSP Loops
**Learning:** Base-10 mathematical functions (like `std::pow(10.0f, ...)` and `std::log10`) are significantly slower than their base-2 equivalents (`std::exp2`, `std::log2`) on the ESP32-P4 architecture.
**Action:** When working in hot audio DSP loops, always replace base-10 functions with base-2 equivalents combined with precomputed constant multipliers.
