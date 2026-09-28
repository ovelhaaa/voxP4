## 2024-05-18 - Compressor optimization
**Learning:** Found that std::pow(10.0f, x) and std::log10(x) are commonly used in audio DSP paths, but we can replace them with std::exp2 and std::log2 multiplied by a constant factor for performance wins.
**Action:** Replace std::pow(10, ...) with std::exp2(...) and std::log10 with std::log2 in DSP loops, and apply floating point tolerance for strict equivalence tests.
