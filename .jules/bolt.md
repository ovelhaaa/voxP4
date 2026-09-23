## 2024-05-18 - Base-2 math replacements
**Learning:** For replacing std::log10 with std::log2 in base-2 performance improvements, use exact constants and floating point tolerance in tests since precision can slightly mismatch.
**Action:** Replace `std::pow(10.0f, ...)` with `std::exp2(...)` and `std::log10(...)` with `std::log2(...)` using appropriate precalculated factors. Make sure to update equivalence tests if they assert exact bitwise equality instead of using a tolerance value.
