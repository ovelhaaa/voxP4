#pragma once

#include <cmath>
#include <cstdint>

constexpr float TEMPO_BPM_MIN = 30.0f;
constexpr float TEMPO_BPM_MAX = 300.0f;
constexpr float TEMPO_BPM_DEFAULT = 120.0f;

enum class TempoSubdivision : uint8_t {
  Whole = 0,         // 1/1  (4.0x quarter)
  Half,              // 1/2  (2.0x quarter)
  Quarter,           // 1/4  (1.0x quarter)
  Eighth,            // 1/8  (0.5x quarter)
  Sixteenth,         // 1/16 (0.25x quarter)
  ThirtySecond,      // 1/32 (0.125x quarter)
  DottedHalf,        // 1/2D (3.0x quarter)
  DottedQuarter,     // 1/4D (1.5x quarter)
  DottedEighth,      // 1/8D (0.75x quarter)
  DottedSixteenth,   // 1/16D (0.375x quarter)
  TripletQuarter,    // 1/4T (2/3x quarter)
  TripletEighth,     // 1/8T (1/3x quarter)
  TripletSixteenth,  // 1/16T (1/6x quarter)
  Count
};

// Clamps BPM to [TEMPO_BPM_MIN, TEMPO_BPM_MAX], replacing NaN / non-finite with default.
inline float tempo_clamp_bpm(float bpm) {
  if (!std::isfinite(bpm)) {
    return TEMPO_BPM_DEFAULT;
  }
  if (bpm < TEMPO_BPM_MIN) {
    return TEMPO_BPM_MIN;
  }
  if (bpm > TEMPO_BPM_MAX) {
    return TEMPO_BPM_MAX;
  }
  return bpm;
}

// Returns the subdivision ratio relative to a quarter note (1/4 = 1.0).
float tempo_subdivision_ratio(TempoSubdivision subdivision);

// Returns the duration of a subdivision in milliseconds at the specified BPM.
// Protected against NaN/Inf and out-of-range BPM.
float tempo_subdivision_ms(float bpm, TempoSubdivision subdivision);

// Human-readable string identifier for subdivisions (e.g. "1/8", "1/8D").
const char *tempo_subdivision_name(TempoSubdivision subdivision);
