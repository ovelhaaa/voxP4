#pragma once
// Coherent, snapshot-able control-plane product state. Values are stored once
// per registered parameter in registry order. Writes happen on the control
// task only; the monotonic revision lets clients detect stale views.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace voxlink {

class ProductState {
public:
  // Loads every registered parameter default and resets revision to 0.
  void init();

  uint32_t revision() const { return revision_.load(std::memory_order_acquire); }

  bool get_value(uint16_t id, float *out) const;
  // Writes an already-normalized, accepted value. Returns true when the stored
  // value changed; the revision increments only on an actual change.
  bool set_value(uint16_t id, float value);

  size_t value_count() const;
  bool value_at(size_t index, uint16_t *id, float *out) const;

private:
  static constexpr size_t kCapacity = 128;
  uint16_t ids_[kCapacity] = {0};
  std::atomic<float> values_[kCapacity];
  size_t count_ = 0;
  std::atomic<uint32_t> revision_{0};
};

} // namespace voxlink
