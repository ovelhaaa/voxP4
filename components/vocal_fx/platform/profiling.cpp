#include "profiling.h"
#include <algorithm>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
uint64_t Profiler::now_us() { return esp_timer_get_time(); }
#else
#include <chrono>
uint64_t Profiler::now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#endif
void Profiler::begin(ProfileSection s) { start_[(size_t)s] = now_us(); }
void Profiler::end(ProfileSection s, uint64_t d) {
  auto &i = stats_[(size_t)s];
  uint64_t u = now_us() - start_[(size_t)s];
  i.calls++;
  i.total_us += u;
  i.max_us = std::max(i.max_us, u);
  if (d && u > d)
    i.deadline_misses++;
}
ProfileStats Profiler::stats(ProfileSection s) const {
  return stats_[(size_t)s];
}
void Profiler::reset() { stats_ = {}; }
