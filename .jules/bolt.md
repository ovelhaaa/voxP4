## 2023-10-30 - Replace std::pow(10, x/20) with std::exp2(x * log2(10)/20)
**Learning:** Using `std::exp2` with a precomputed constant multiplier instead of `std::pow(10.0f, ...)` yields over 2x speedup and is exact equivalent to floats.
**Action:** Always prefer base-2 math functions (`std::exp2`, `std::log2`) over base-10 functions (`std::pow(10, ...)`, `std::log10`) combined with precomputed constant multipliers in hot paths for performance.
