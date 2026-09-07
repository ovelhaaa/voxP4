#pragma once
#include <array>
#include <atomic>
#include <cstdint>

class MidiChordState {
public:
  void note_on(uint8_t note, uint8_t velocity);
  void note_off(uint8_t note);
  void all_notes_off();
  uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
  std::array<bool,128> snapshot() const;
private:
  std::array<std::atomic<uint8_t>,128> held_{};
  std::atomic<uint32_t> generation_{0};
};

