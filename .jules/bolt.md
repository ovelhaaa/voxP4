## 2024-10-24 - DSP Hot Path Optimization Insight
**Learning:** On the ESP32-P4 architecture, base-10 functions like `std::pow(10.0f, ...)` and `std::log10(...)` are computationally expensive in hot audio DSP loops.
**Action:** When optimizing such bottlenecks, replace them with their base-2 equivalents (`std::exp2` and `std::log2`) combined with precomputed constant multipliers for significant performance gains, ensuring equivalence checks allow a small floating-point tolerance (e.g. `1e-4f`).
