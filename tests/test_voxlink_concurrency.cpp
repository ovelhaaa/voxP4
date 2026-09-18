// Concurrency safety of the control-plane shadow state and the existing
// bounded SPSC parameter queue. There is one writer (control) and one reader
// (simulated audio/telemetry); no torn values, no lost accepted updates.
#include "parameter_queue.h"
#include "voxlink_state.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace voxlink;

namespace {
int g_failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}
} // namespace

int main() {
  // ProductState: atomic per-value writes, monotonic revision.
  {
    ProductState state;
    state.init();
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    std::thread writer([&] {
      const float values[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
      for (int i = 0; i < 200000; ++i) {
        state.set_value(0x0401, values[i % 5]);
      }
      stop.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
      while (!stop.load(std::memory_order_acquire)) {
        float v = -1.0f;
        if (state.get_value(0x0401, &v)) {
          if (!(v >= 0.0f && v <= 1.0f))
            bad.store(true, std::memory_order_relaxed);
        }
      }
    });

    writer.join();
    reader.join();
    check(!bad.load(), "no torn/out-of-range value observed");
    check(state.revision() > 0, "revision advanced");
  }

  // Bounded SPSC queue: producer never blocks, consumer drains, and every
  // accepted entry is delivered exactly once.
  {
    ParameterQueue<8> queue;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> bad_value{false};

    std::thread producer([&] {
      for (int i = 0; i < 200000; ++i) {
        const float v = (i % 4) * 0.25f;
        if (queue.push({VocalFxParameter::ReverbWet, v}))
          accepted.fetch_add(1, std::memory_order_relaxed);
        else
          rejected.fetch_add(1, std::memory_order_relaxed);
      }
      stop.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
      ParameterChange change;
      for (;;) {
        if (queue.pop(change)) {
          consumed.fetch_add(1, std::memory_order_relaxed);
          if (change.parameter != VocalFxParameter::ReverbWet)
            bad_value.store(true, std::memory_order_relaxed);
          const float f = change.value;
          if (!(f == 0.0f || f == 0.25f || f == 0.5f || f == 0.75f))
            bad_value.store(true, std::memory_order_relaxed);
          continue;
        }
        if (stop.load(std::memory_order_acquire) &&
            consumed.load(std::memory_order_relaxed) >=
                accepted.load(std::memory_order_relaxed))
          break;
        std::this_thread::yield();
      }
    });

    producer.join();
    consumer.join();
    check(!bad_value.load(), "queue delivered only valid entries");
    check(consumed.load() == accepted.load(),
          "every accepted entry drained exactly once");
    check(consumed.load() > 0 && rejected.load() > 0,
          "queue exercised both accept and reject paths");
  }

  if (g_failures == 0)
    std::puts("voxlink_concurrency_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
