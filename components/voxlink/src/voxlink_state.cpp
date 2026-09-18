#include "voxlink_state.h"
#include "voxlink_registry.h"

namespace voxlink {

void ProductState::init() {
  const ParamDescriptor *params = registry_params();
  const size_t n = registry_count();
  count_ = (n < kCapacity) ? n : kCapacity;
  for (size_t i = 0; i < count_; ++i) {
    ids_[i] = params[i].id;
    values_[i].store(params[i].default_value, std::memory_order_relaxed);
  }
  revision_.store(0, std::memory_order_release);
}

bool ProductState::get_value(uint16_t id, float *out) const {
  if (out == nullptr)
    return false;
  for (size_t i = 0; i < count_; ++i) {
    if (ids_[i] == id) {
      *out = values_[i].load(std::memory_order_acquire);
      return true;
    }
  }
  return false;
}

bool ProductState::set_value(uint16_t id, float value) {
  for (size_t i = 0; i < count_; ++i) {
    if (ids_[i] == id) {
      const float previous = values_[i].load(std::memory_order_relaxed);
      if (previous == value)
        return false;
      values_[i].store(value, std::memory_order_release);
      revision_.fetch_add(1, std::memory_order_acq_rel);
      return true;
    }
  }
  return false;
}

size_t ProductState::value_count() const { return count_; }

bool ProductState::value_at(size_t index, uint16_t *id, float *out) const {
  if (index >= count_)
    return false;
  if (id != nullptr)
    *id = ids_[index];
  if (out != nullptr)
    *out = values_[index].load(std::memory_order_acquire);
  return true;
}

} // namespace voxlink
