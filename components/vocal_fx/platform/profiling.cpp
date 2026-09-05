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
  const size_t index = (size_t)s;
  auto &i = stats_[index];
  uint64_t u = now_us() - start_[(size_t)s];
  i.calls++;
  i.total_us += u;
  i.max_us = std::max(i.max_us, u);
  if (d && u > d)
    i.deadline_misses++;
  publish(index);
}
ProfileStats Profiler::stats(ProfileSection s) const {
  const auto &source = published_[(size_t)s];
  ProfileStats result;
  uint32_t before, after;
  do {
    before = source.sequence.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    result.calls = load(source.calls);
    result.total_us = load(source.total_us);
    result.max_us = load(source.max_us);
    result.deadline_misses = load(source.deadline_misses);
    after = source.sequence.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return result;
}
void Profiler::store(Atomic64Parts &destination, uint64_t value) {
  destination.low.store((uint32_t)value, std::memory_order_relaxed);
  destination.high.store((uint32_t)(value >> 32U), std::memory_order_relaxed);
}
uint64_t Profiler::load(const Atomic64Parts &source) {
  const uint64_t low = source.low.load(std::memory_order_relaxed);
  const uint64_t high = source.high.load(std::memory_order_relaxed);
  return low | (high << 32U);
}
void Profiler::publish(size_t index) {
  auto &destination = published_[index];
  destination.sequence.fetch_add(1, std::memory_order_acq_rel);
  const auto &source = stats_[index];
  store(destination.calls, source.calls);
  store(destination.total_us, source.total_us);
  store(destination.max_us, source.max_us);
  store(destination.deadline_misses, source.deadline_misses);
  destination.sequence.fetch_add(1, std::memory_order_release);
}
void Profiler::reset() {
  stats_ = {};
  for (size_t index = 0; index < stats_.size(); ++index)
    publish(index);
}
