#pragma once

#include "vocal_fx_types.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct ParameterChange {
  VocalFxParameter parameter;
  float value;
};

// Single-producer/single-consumer queue. The UI/control task is the producer
// and the audio task is the consumer. A full queue rejects the newest update
// rather than blocking either task or overwriting an update being consumed.
template <size_t Capacity> class ParameterQueue {
  static_assert(Capacity > 1, "queue needs one sentinel slot");
  static_assert(ATOMIC_INT_LOCK_FREE == 2,
                "audio parameter indices must be lock-free");

public:
  bool push(ParameterChange change) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t next = increment(head);
    if (next == tail_.load(std::memory_order_acquire))
      return false;
    entries_[head] = change;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(ParameterChange &change) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire))
      return false;
    change = entries_[tail];
    tail_.store(increment(tail), std::memory_order_release);
    return true;
  }

  void reset() {
    const uint32_t head = head_.load(std::memory_order_acquire);
    tail_.store(head, std::memory_order_release);
  }

private:
  static constexpr uint32_t increment(uint32_t index) {
    return (index + 1U) % Capacity;
  }

  std::array<ParameterChange, Capacity> entries_{};
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
};
