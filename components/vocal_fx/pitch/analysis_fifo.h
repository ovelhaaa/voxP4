#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct AnalysisSample {
  float value;
  uint64_t input_position;
};

template <size_t Capacity, typename Sample = AnalysisSample> class AnalysisFifo {
public:
  bool push(Sample value) {
    pushes_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1U) % Capacity;
    if (next == tail_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      overflow_attempts_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    data_[h] = value;
    head_.store(next, std::memory_order_release);
    record_occupancy(distance(next, tail_.load(std::memory_order_acquire)));
    return true;
  }
  bool pop(Sample &value) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire))
      return false;
    value = data_[t];
    const uint32_t next = (t + 1U) % Capacity;
    tail_.store(next, std::memory_order_release);
    pops_.fetch_add(1, std::memory_order_relaxed);
    record_occupancy(distance(head_.load(std::memory_order_acquire), next));
    return true;
  }
  void reset() {
    tail_.store(head_.load(std::memory_order_acquire),
                std::memory_order_release);
    dropped_.store(0, std::memory_order_relaxed);
    pushes_.store(0, std::memory_order_relaxed);
    pops_.store(0, std::memory_order_relaxed);
    overflow_attempts_.store(0, std::memory_order_relaxed);
    occupancy_sum_.store(0, std::memory_order_relaxed);
    occupancy_observations_.store(0, std::memory_order_relaxed);
    occupancy_sample_sequence_.store(0, std::memory_order_relaxed);
    maximum_occupancy_.store(0, std::memory_order_relaxed);
  }
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  uint64_t pushes() const { return pushes_.load(std::memory_order_relaxed); }
  uint64_t pops() const { return pops_.load(std::memory_order_relaxed); }
  uint64_t overflow_attempts() const {
    return overflow_attempts_.load(std::memory_order_relaxed);
  }
  uint32_t occupancy() const {
    return distance(head_.load(std::memory_order_acquire),
                    tail_.load(std::memory_order_acquire));
  }
  uint32_t maximum_occupancy() const {
    return maximum_occupancy_.load(std::memory_order_relaxed);
  }
  double average_occupancy() const {
    const uint64_t observations =
        occupancy_observations_.load(std::memory_order_relaxed);
    return observations
               ? static_cast<double>(occupancy_sum_.load(
                     std::memory_order_relaxed)) /
                     static_cast<double>(observations)
               : 0.0;
  }

private:
  static uint32_t distance(uint32_t head, uint32_t tail) {
    return head >= tail ? head - tail : head + Capacity - tail;
  }
  void record_occupancy(uint32_t occupancy) {
    uint32_t maximum = maximum_occupancy_.load(std::memory_order_relaxed);
    while (occupancy > maximum &&
           !maximum_occupancy_.compare_exchange_weak(
               maximum, occupancy, std::memory_order_relaxed)) {
    }
    // Sample one in 64 SPSC operations so passive average-occupancy telemetry
    // does not add multiple atomic operations to every analysis sample.
    const uint32_t sequence =
        occupancy_sample_sequence_.fetch_add(1, std::memory_order_relaxed);
    if ((sequence & 63U) == 0U) {
      occupancy_sum_.fetch_add(occupancy, std::memory_order_relaxed);
      occupancy_observations_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::array<Sample, Capacity> data_{};
  std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
  std::atomic<uint32_t> pushes_{0}, pops_{0}, overflow_attempts_{0};
  std::atomic<uint32_t> occupancy_sum_{0}, occupancy_observations_{0};
  std::atomic<uint32_t> occupancy_sample_sequence_{0}, maximum_occupancy_{0};
};
