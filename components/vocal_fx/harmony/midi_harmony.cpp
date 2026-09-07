#include "midi_harmony.h"
namespace {
void begin_update(std::atomic<uint32_t> &generation) {
  generation.fetch_add(1, std::memory_order_acq_rel);
}
void end_update(std::atomic<uint32_t> &generation) {
  generation.fetch_add(1, std::memory_order_release);
}
} // namespace

void MidiChordState::note_on(uint8_t note, uint8_t velocity) {
  if (note >= held_.size())
    return;
  begin_update(generation_);
  held_[note].store(velocity ? 1 : 0, std::memory_order_relaxed);
  end_update(generation_);
}
void MidiChordState::note_off(uint8_t note) {
  if (note >= held_.size())
    return;
  begin_update(generation_);
  held_[note].store(0, std::memory_order_relaxed);
  end_update(generation_);
}
void MidiChordState::all_notes_off() {
  begin_update(generation_);
  for (auto &note : held_)
    note.store(0, std::memory_order_relaxed);
  end_update(generation_);
}
std::array<bool, 128> MidiChordState::snapshot() const {
  std::array<bool, 128> result{};
  uint32_t before = 0, after = 0;
  do {
    before = generation_.load(std::memory_order_acquire);
    if (before & 1U)
      continue;
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = held_[i].load(std::memory_order_relaxed) != 0;
    after = generation_.load(std::memory_order_acquire);
  } while ((before & 1U) || before != after);
  return result;
}
