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
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1U) % Capacity;
    if (next == tail_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    data_[h] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }
  bool pop(Sample &value) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire))
      return false;
    value = data_[t];
    tail_.store((t + 1U) % Capacity, std::memory_order_release);
    return true;
  }
  void reset() {
    tail_.store(head_.load(std::memory_order_acquire),
                std::memory_order_release);
    dropped_.store(0, std::memory_order_relaxed);
  }
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
  std::array<Sample, Capacity> data_{};
  std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
};
