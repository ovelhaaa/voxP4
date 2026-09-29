## 2023-09-29 - [Optimization of power function]
**Learning:** Found that std::pow(10.0f, x) is significantly slower and can be optimized by using std::exp2(x * kDbToLog2) while retaining bitwise equivalence for audio hotpaths on ESP32-P4 architecture.
**Action:** Consistently replace power base 10 by exponential base 2 multiplied by appropriate constants where applicable.
