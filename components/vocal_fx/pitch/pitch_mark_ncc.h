#pragma once

#include <cstddef>
#include <cstdint>
#include "vocal_fx_config.h"

// Stage B4B.4F keeps the pre-existing scalar NCC as an explicit oracle.  The
// remaining variants change only how the same candidate scores are computed.
const char *pitch_mark_ncc_variant_name(PitchMarkNccVariant variant);

// A and the union of all B windows share caller-owned storage. PitchAnalysis
// reuses its existing 1024-sample linear YIN window, adding no production RAM.
static constexpr size_t kPitchMarkNccScratchCapacity = 1024;

struct PitchMarkNccResult {
  float best_score = -2.0f;
  int best_offset = 0;
  uint32_t offsets = 0;
  uint64_t sample_pairs = 0;
};

// Searches [-radius, radius]. ring_size must be a power of two. When scores is
// non-null, one score per offset is written in increasing-offset order.
PitchMarkNccResult pitch_mark_ncc_search(
    PitchMarkNccVariant variant, const float *ring, size_t ring_size,
    uint64_t previous_mark, uint64_t predicted_mark, int radius, int window,
    float *scratch = nullptr, size_t scratch_capacity = 0,
    float *scores = nullptr, size_t score_capacity = 0);
