#include "pitch_mark_ncc.h"

#include <algorithm>
#include <cmath>

namespace {
inline float ring_at(const float *ring, size_t mask, uint64_t position) {
  return ring[position & mask];
}

inline float finish_score(float dot, float aa, float bb) {
  return dot / std::sqrt(std::max(aa * bb, 1e-20f));
}

float dot_fma4(const float *ring, size_t mask, uint64_t a_start,
               uint64_t b_start, int window) {
  float d0 = 0.0f, d1 = 0.0f, d2 = 0.0f, d3 = 0.0f;
  int i = 0;
  for (; i + 3 < window; i += 4) {
    d0 = std::fma(ring_at(ring, mask, a_start + i),
                  ring_at(ring, mask, b_start + i), d0);
    d1 = std::fma(ring_at(ring, mask, a_start + i + 1),
                  ring_at(ring, mask, b_start + i + 1), d1);
    d2 = std::fma(ring_at(ring, mask, a_start + i + 2),
                  ring_at(ring, mask, b_start + i + 2), d2);
    d3 = std::fma(ring_at(ring, mask, a_start + i + 3),
                  ring_at(ring, mask, b_start + i + 3), d3);
  }
  float dot = (d0 + d1) + (d2 + d3);
  for (; i < window; ++i)
    dot = std::fma(ring_at(ring, mask, a_start + i),
                   ring_at(ring, mask, b_start + i), dot);
  return dot;
}

float dot_fma8(const float *ring, size_t mask, uint64_t a_start,
               uint64_t b_start, int window) {
  float d0 = 0.0f, d1 = 0.0f, d2 = 0.0f, d3 = 0.0f;
  float d4 = 0.0f, d5 = 0.0f, d6 = 0.0f, d7 = 0.0f;
  int i = 0;
  for (; i + 7 < window; i += 8) {
    d0 = std::fma(ring_at(ring, mask, a_start + i),
                  ring_at(ring, mask, b_start + i), d0);
    d1 = std::fma(ring_at(ring, mask, a_start + i + 1),
                  ring_at(ring, mask, b_start + i + 1), d1);
    d2 = std::fma(ring_at(ring, mask, a_start + i + 2),
                  ring_at(ring, mask, b_start + i + 2), d2);
    d3 = std::fma(ring_at(ring, mask, a_start + i + 3),
                  ring_at(ring, mask, b_start + i + 3), d3);
    d4 = std::fma(ring_at(ring, mask, a_start + i + 4),
                  ring_at(ring, mask, b_start + i + 4), d4);
    d5 = std::fma(ring_at(ring, mask, a_start + i + 5),
                  ring_at(ring, mask, b_start + i + 5), d5);
    d6 = std::fma(ring_at(ring, mask, a_start + i + 6),
                  ring_at(ring, mask, b_start + i + 6), d6);
    d7 = std::fma(ring_at(ring, mask, a_start + i + 7),
                  ring_at(ring, mask, b_start + i + 7), d7);
  }
  float dot = ((d0 + d1) + (d2 + d3)) + ((d4 + d5) + (d6 + d7));
  for (; i < window; ++i)
    dot = std::fma(ring_at(ring, mask, a_start + i),
                   ring_at(ring, mask, b_start + i), dot);
  return dot;
}
} // namespace

const char *pitch_mark_ncc_variant_name(PitchMarkNccVariant variant) {
  switch (variant) {
  case PitchMarkNccVariant::Reference: return "PITCH_MARK_NCC_REFERENCE";
  case PitchMarkNccVariant::ReuseAa: return "PITCH_MARK_NCC_REUSE_AA";
  case PitchMarkNccVariant::Fma4AccDot: return "PITCH_MARK_NCC_FMA_4ACC";
  case PitchMarkNccVariant::Fma8AccDot: return "PITCH_MARK_NCC_FMA_8ACC";
  case PitchMarkNccVariant::Fma8SlidingBb:
    return "PITCH_MARK_NCC_FMA8_SLIDING_BB";
  case PitchMarkNccVariant::LinearScratch:
    return "PITCH_MARK_NCC_LINEAR_SCRATCH";
  }
  return "PITCH_MARK_NCC_REFERENCE";
}

PitchMarkNccResult pitch_mark_ncc_search(
    PitchMarkNccVariant variant, const float *ring, size_t ring_size,
    uint64_t previous_mark, uint64_t predicted_mark, int radius, int window,
    float *scratch, size_t scratch_capacity, float *scores,
    size_t score_capacity) {
  PitchMarkNccResult result{};
  if (!ring || ring_size == 0 || (ring_size & (ring_size - 1)) != 0 ||
      radius < 0 || window <= 0 || previous_mark < static_cast<uint64_t>(window))
    return result;

  const size_t mask = ring_size - 1;
  const uint64_t a_start = previous_mark - static_cast<uint64_t>(window);
  const int64_t first_candidate = static_cast<int64_t>(predicted_mark) - radius;
  const bool use_linear = variant == PitchMarkNccVariant::LinearScratch &&
                          scratch &&
                          static_cast<size_t>(2 * window + 2 * radius) <=
                              scratch_capacity &&
                          first_candidate >= window;
  const PitchMarkNccVariant effective_variant =
      variant == PitchMarkNccVariant::LinearScratch && !use_linear
          ? PitchMarkNccVariant::ReuseAa
          : variant;
  if (use_linear) {
    const uint64_t b_region =
        static_cast<uint64_t>(first_candidate - window);
    for (int i = 0; i < window; ++i)
      scratch[i] = ring_at(ring, mask, a_start + i);
    for (int i = 0; i < window + 2 * radius; ++i)
      scratch[window + i] = ring_at(ring, mask, b_region + i);
  }

  float aa_reused = 0.0f;
  if (effective_variant != PitchMarkNccVariant::Reference) {
    for (int i = 0; i < window; ++i) {
      const float a = use_linear ? scratch[i]
                                 : ring_at(ring, mask, a_start + i);
      aa_reused += a * a;
    }
  }

  float sliding_bb = 0.0f;
  bool have_sliding_bb = false;
  size_t score_index = 0;
  for (int offset = -radius; offset <= radius; ++offset, ++score_index) {
    const int64_t candidate = static_cast<int64_t>(predicted_mark) + offset;
    if (candidate < window) {
      if (scores && score_index < score_capacity)
        scores[score_index] = -2.0f;
      continue;
    }
    ++result.offsets;
    result.sample_pairs += static_cast<uint64_t>(window);
    const uint64_t b_start = static_cast<uint64_t>(candidate - window);
    float dot = 0.0f, aa = aa_reused, bb = 0.0f;
    if (effective_variant == PitchMarkNccVariant::Reference) {
      aa = 0.0f;
      for (int i = 0; i < window; ++i) {
        const float a = ring_at(ring, mask, a_start + i);
        const float b = ring_at(ring, mask, b_start + i);
        dot += a * b;
        aa += a * a;
        bb += b * b;
      }
    } else if (use_linear) {
      const int b_offset = offset + radius;
      for (int i = 0; i < window; ++i) {
        const float a = scratch[i];
        const float b = scratch[window + b_offset + i];
        dot += a * b;
        bb += b * b;
      }
    } else {
      if (effective_variant == PitchMarkNccVariant::Fma4AccDot)
        dot = dot_fma4(ring, mask, a_start, b_start, window);
      else
        dot = (effective_variant == PitchMarkNccVariant::Fma8AccDot ||
               effective_variant == PitchMarkNccVariant::Fma8SlidingBb)
                  ? dot_fma8(ring, mask, a_start, b_start, window)
                  : 0.0f;

      if (effective_variant == PitchMarkNccVariant::ReuseAa) {
        for (int i = 0; i < window; ++i) {
          const float a = ring_at(ring, mask, a_start + i);
          const float b = ring_at(ring, mask, b_start + i);
          dot += a * b;
          bb += b * b;
        }
      } else if (effective_variant == PitchMarkNccVariant::Fma8SlidingBb &&
                 have_sliding_bb) {
        const float leaving = ring_at(ring, mask, b_start - 1);
        const float entering = ring_at(ring, mask, b_start + window - 1);
        sliding_bb = sliding_bb - leaving * leaving + entering * entering;
        bb = sliding_bb;
      } else {
        for (int i = 0; i < window; ++i) {
          const float b = ring_at(ring, mask, b_start + i);
          bb += b * b;
        }
        if (effective_variant == PitchMarkNccVariant::Fma8SlidingBb) {
          sliding_bb = bb;
          have_sliding_bb = true;
        }
      }
    }
    const float score = finish_score(dot, aa, bb);
    if (scores && score_index < score_capacity)
      scores[score_index] = score;
    if (score > result.best_score) {
      result.best_score = score;
      result.best_offset = offset;
    }
  }
  return result;
}
