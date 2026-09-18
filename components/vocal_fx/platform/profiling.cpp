#include "profiling.h"
#include <algorithm>
#include <cstdlib>
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
uint64_t Profiler::now_us() { return esp_timer_get_time(); }
uint32_t Profiler::now_cycles() { return esp_cpu_get_cycle_count(); }
uint32_t Profiler::cycles_per_us() { return 360; }
#else
#include <chrono>
uint64_t Profiler::now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
uint32_t Profiler::now_cycles() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
uint32_t Profiler::cycles_per_us() { return 1000; }
#endif
namespace {
constexpr size_t kDistributionCapacity = 512;
constexpr size_t kDistributionOwners = 2;
struct DistributionSlot {
  const Profiler *owner = nullptr;
  uint16_t *values = nullptr;
  uint32_t *seen = nullptr;
  size_t first = 0;
  size_t count = 0;
};
struct DistributionRegistry {
  DistributionSlot slots[kDistributionOwners]{};
};
DistributionRegistry *s_distribution_registry = nullptr;

void *distribution_calloc(size_t count, size_t size) {
#ifdef ESP_PLATFORM
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return std::calloc(count, size);
#endif
}
void distribution_free(void *pointer) {
#ifdef ESP_PLATFORM
  heap_caps_free(pointer);
#else
  std::free(pointer);
#endif
}
} // namespace
Profiler::~Profiler() {
  if (!s_distribution_registry)
    return;
  for (auto &slot : s_distribution_registry->slots) {
    if (slot.owner != this)
      continue;
    distribution_free(slot.values);
    distribution_free(slot.seen);
    slot = {};
    return;
  }
}
bool Profiler::enable_distribution_range(ProfileSection first,
                                         ProfileSection last) {
  const size_t first_index = static_cast<size_t>(first);
  const size_t last_index = static_cast<size_t>(last);
  if (last_index < first_index ||
      last_index >= static_cast<size_t>(ProfileSection::Count))
    return false;
  if (!s_distribution_registry)
    s_distribution_registry = static_cast<DistributionRegistry *>(
        distribution_calloc(1, sizeof(DistributionRegistry)));
  if (!s_distribution_registry)
    return false;
  DistributionSlot *selected = nullptr;
  for (auto &slot : s_distribution_registry->slots)
    if (slot.owner == this || (!slot.owner && !selected))
      selected = &slot;
  if (!selected)
    return false;
  distribution_free(selected->values);
  distribution_free(selected->seen);
  selected->owner = this;
  selected->first = first_index;
  selected->count = last_index - first_index + 1;
  selected->values = static_cast<uint16_t *>(distribution_calloc(
      selected->count * kDistributionCapacity, sizeof(uint16_t)));
  selected->seen = static_cast<uint32_t *>(
      distribution_calloc(selected->count, sizeof(uint32_t)));
  if (!selected->values || !selected->seen) {
    distribution_free(selected->values);
    distribution_free(selected->seen);
    *selected = {};
    return false;
  }
  return true;
}
void Profiler::record_distribution(size_t index, uint64_t elapsed_us) {
  if (!s_distribution_registry)
    return;
  for (auto &slot : s_distribution_registry->slots) {
    if (slot.owner != this || index < slot.first ||
        index >= slot.first + slot.count)
      continue;
    const size_t local = index - slot.first;
    const uint32_t seen = slot.seen[local]++;
    slot.values[local * kDistributionCapacity +
                (seen % kDistributionCapacity)] =
        static_cast<uint16_t>(std::min<uint64_t>(elapsed_us, UINT16_MAX));
    return;
  }
}
#if VOCAL_FX_ENABLE_PROFILING
void Profiler::begin(ProfileSection s) {
  const size_t index = static_cast<size_t>(s);
  start_[index] = now_us();
  start_cycles_[index] = now_cycles();
}
void Profiler::end(ProfileSection s, uint64_t d) {
  const size_t index = (size_t)s;
  auto &i = stats_[index];
  uint64_t u = now_us() - start_[(size_t)s];
  const uint64_t cycles = static_cast<uint32_t>(
      now_cycles() - start_cycles_[index]);
  i.calls++;
  i.total_us += u;
  i.max_us = std::max(i.max_us, u);
  i.total_cycles += cycles;
  i.max_cycles = std::max(i.max_cycles, cycles);
  if (d && u > d)
    i.deadline_misses++;
  record_distribution(index, u);
  publish(index);
}
void Profiler::record_cycles(ProfileSection s, uint64_t cycles,
                             uint64_t calls) {
  if (calls == 0)
    return;
  const size_t index = static_cast<size_t>(s);
  auto &stats = stats_[index];
  const uint64_t elapsed_us = cycles / cycles_per_us();
  stats.calls += calls;
  stats.total_cycles += cycles;
  stats.total_us += elapsed_us;
  stats.max_cycles = std::max(stats.max_cycles, cycles);
  stats.max_us = std::max(stats.max_us, elapsed_us);
  record_distribution(index, elapsed_us);
  publish(index);
}
#else
// B4D.6 production-equivalent build: the detailed profiler is compiled out.
void Profiler::begin(ProfileSection) {}
void Profiler::end(ProfileSection, uint64_t) {}
void Profiler::record_cycles(ProfileSection, uint64_t, uint64_t) {}
#endif
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
    result.total_cycles = load(source.total_cycles);
    result.max_cycles = load(source.max_cycles);
    after = source.sequence.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return result;
}
ProfileDistributionStats
Profiler::distribution_stats(ProfileSection s) const {
  ProfileDistributionStats result{};
  const size_t index = static_cast<size_t>(s);
  if (!s_distribution_registry)
    return result;
  for (const auto &slot : s_distribution_registry->slots) {
    if (slot.owner != this || index < slot.first ||
        index >= slot.first + slot.count)
      continue;
    const size_t local = index - slot.first;
    const size_t count =
        std::min<size_t>(slot.seen[local], kDistributionCapacity);
    std::array<uint16_t, kDistributionCapacity> sorted{};
    const uint16_t *source = slot.values + local * kDistributionCapacity;
    std::copy_n(source, count, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + count);
    const auto percentile = [&](size_t percent) -> uint32_t {
      if (!count)
        return 0;
      const size_t percentile_index = std::min<size_t>(
          count - 1, (count * percent + 99) / 100 - 1);
      return sorted[percentile_index];
    };
    result.p50_us = percentile(50);
    result.p95_us = percentile(95);
    result.p99_us = percentile(99);
    result.samples = static_cast<uint32_t>(count);
    break;
  }
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
  store(destination.total_cycles, source.total_cycles);
  store(destination.max_cycles, source.max_cycles);
  destination.sequence.fetch_add(1, std::memory_order_release);
}
void Profiler::reset() {
  stats_ = {};
  if (s_distribution_registry)
    for (auto &slot : s_distribution_registry->slots)
      if (slot.owner == this) {
        std::fill_n(slot.values, slot.count * kDistributionCapacity,
                    uint16_t{0});
        std::fill_n(slot.seen, slot.count, uint32_t{0});
      }
  for (size_t index = 0; index < stats_.size(); ++index)
    publish(index);
}
