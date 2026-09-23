## 2024-05-24 - Replace base-10 math functions with base-2 on ESP32-P4
**Learning:** The ESP32-P4 floating-point unit handles base-2 functions like `std::exp2` and `std::log2` much more efficiently than base-10 functions like `std::pow(10.0f, x)` or `std::log10`. Replacing these with precomputed constant multipliers in hot audio DSP loops yields significant performance gains.
**Action:** In DSP hot paths, avoid `std::pow`, `std::log10`, or `std::exp`. Always prefer `std::exp2` and `std::log2` and multiply by precomputed constant ratios to achieve the desired mathematical result.
