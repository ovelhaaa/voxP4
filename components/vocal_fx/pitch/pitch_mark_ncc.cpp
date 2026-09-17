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

inline float fma_acc(float a, float b, float c) {
#if defined(__riscv)
  return __builtin_fmaf(a, b, c);
#else
  return a * b + c;
#endif
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

template <int MultiWidth>
PitchMarkNccResult search_contiguous_multi(
    const float *ring, size_t ring_size,
    uint64_t previous_mark, uint64_t predicted_mark, int radius, int window,
    float *scratch, size_t scratch_capacity, float *scores,
    size_t score_capacity) {
  PitchMarkNccResult result{};
  const size_t mask = ring_size - 1;
  const uint64_t a_start = previous_mark - static_cast<uint64_t>(window);
  const int64_t first_candidate = static_cast<int64_t>(predicted_mark) - radius;
  if (first_candidate < window) {
    return pitch_mark_ncc_search(PitchMarkNccVariant::ReuseAa, ring, ring_size,
                                 previous_mark, predicted_mark, radius, window,
                                 scratch, scratch_capacity, scores, score_capacity);
  }

  const size_t a_pos = static_cast<size_t>(a_start) & mask;
  const bool a_contiguous = (a_pos + static_cast<size_t>(window)) <= ring_size;

  const uint64_t b_region_start = static_cast<uint64_t>(first_candidate - window);
  const size_t b_total_len = static_cast<size_t>(2 * radius + window);
  const size_t b_pos = static_cast<size_t>(b_region_start) & mask;
  const bool b_contiguous = (b_pos + b_total_len) <= ring_size;

  const float *a_ptr = nullptr;
  float a_local[512];
  if (a_contiguous) {
    a_ptr = ring + a_pos;
  } else {
    for (int i = 0; i < window; ++i)
      a_local[i] = ring_at(ring, mask, a_start + i);
    a_ptr = a_local;
  }

  const float *b_ptr = nullptr;
  float b_local[1024];
  if (b_contiguous) {
    b_ptr = ring + b_pos;
  } else {
    float *b_dest = (scratch && scratch_capacity >= b_total_len) ? scratch : b_local;
    for (size_t i = 0; i < b_total_len; ++i)
      b_dest[i] = ring_at(ring, mask, b_region_start + i);
    b_ptr = b_dest;
  }

  float aa = 0.0f;
  for (int i = 0; i < window; ++i) {
    const float a_val = a_ptr[i];
    aa += a_val * a_val;
  }

  const int total_candidates = 2 * radius + 1;
  int offset_idx = 0;

  if constexpr (MultiWidth >= 8) {
    for (; offset_idx + 7 < total_candidates; offset_idx += 8) {
      const int off0 = -radius + offset_idx;
      const int off1 = off0 + 1;
      const int off2 = off0 + 2;
      const int off3 = off0 + 3;
      const int off4 = off0 + 4;
      const int off5 = off0 + 5;
      const int off6 = off0 + 6;
      const int off7 = off0 + 7;

      const float *__restrict b0 = b_ptr + offset_idx;

      float dot0 = 0.0f, dot1 = 0.0f, dot2 = 0.0f, dot3 = 0.0f;
      float dot4 = 0.0f, dot5 = 0.0f, dot6 = 0.0f, dot7 = 0.0f;
      float bb0  = 0.0f, bb1  = 0.0f, bb2  = 0.0f, bb3  = 0.0f;
      float bb4  = 0.0f, bb5  = 0.0f, bb6  = 0.0f, bb7  = 0.0f;

      float v0 = b0[0];
      float v1 = b0[1];
      float v2 = b0[2];
      float v3 = b0[3];
      float v4 = b0[4];
      float v5 = b0[5];
      float v6 = b0[6];

      int i = 0;
      for (; i + 1 < window; i += 2) {
        const float a0 = a_ptr[i];
        const float v7 = b0[i + 7];

        dot0 = fma_acc(a0, v0, dot0); bb0 = fma_acc(v0, v0, bb0);
        dot1 = fma_acc(a0, v1, dot1); bb1 = fma_acc(v1, v1, bb1);
        dot2 = fma_acc(a0, v2, dot2); bb2 = fma_acc(v2, v2, bb2);
        dot3 = fma_acc(a0, v3, dot3); bb3 = fma_acc(v3, v3, bb3);
        dot4 = fma_acc(a0, v4, dot4); bb4 = fma_acc(v4, v4, bb4);
        dot5 = fma_acc(a0, v5, dot5); bb5 = fma_acc(v5, v5, bb5);
        dot6 = fma_acc(a0, v6, dot6); bb6 = fma_acc(v6, v6, bb6);
        dot7 = fma_acc(a0, v7, dot7); bb7 = fma_acc(v7, v7, bb7);

        const float a1 = a_ptr[i + 1];
        const float v8 = b0[i + 8];

        dot0 = fma_acc(a1, v1, dot0); bb0 = fma_acc(v1, v1, bb0);
        dot1 = fma_acc(a1, v2, dot1); bb1 = fma_acc(v2, v2, bb1);
        dot2 = fma_acc(a1, v3, dot2); bb2 = fma_acc(v3, v3, bb2);
        dot3 = fma_acc(a1, v4, dot3); bb3 = fma_acc(v4, v4, bb3);
        dot4 = fma_acc(a1, v5, dot4); bb4 = fma_acc(v5, v5, bb4);
        dot5 = fma_acc(a1, v6, dot5); bb5 = fma_acc(v6, v6, bb5);
        dot6 = fma_acc(a1, v7, dot6); bb6 = fma_acc(v7, v7, bb6);
        dot7 = fma_acc(a1, v8, dot7); bb7 = fma_acc(v8, v8, bb7);

        v0 = v2;
        v1 = v3;
        v2 = v4;
        v3 = v5;
        v4 = v6;
        v5 = v7;
        v6 = v8;
      }
      for (; i < window; ++i) {
        const float a_val = a_ptr[i];
        const float v7 = b0[i + 7];

        dot0 = fma_acc(a_val, v0, dot0); bb0 = fma_acc(v0, v0, bb0);
        dot1 = fma_acc(a_val, v1, dot1); bb1 = fma_acc(v1, v1, bb1);
        dot2 = fma_acc(a_val, v2, dot2); bb2 = fma_acc(v2, v2, bb2);
        dot3 = fma_acc(a_val, v3, dot3); bb3 = fma_acc(v3, v3, bb3);
        dot4 = fma_acc(a_val, v4, dot4); bb4 = fma_acc(v4, v4, bb4);
        dot5 = fma_acc(a_val, v5, dot5); bb5 = fma_acc(v5, v5, bb5);
        dot6 = fma_acc(a_val, v6, dot6); bb6 = fma_acc(v6, v6, bb6);
        dot7 = fma_acc(a_val, v7, dot7); bb7 = fma_acc(v7, v7, bb7);

        v0 = v1;
        v1 = v2;
        v2 = v3;
        v3 = v4;
        v4 = v5;
        v5 = v6;
        v6 = v7;
      }

      const float s0 = finish_score(dot0, aa, bb0);
      const float s1 = finish_score(dot1, aa, bb1);
      const float s2 = finish_score(dot2, aa, bb2);
      const float s3 = finish_score(dot3, aa, bb3);
      const float s4 = finish_score(dot4, aa, bb4);
      const float s5 = finish_score(dot5, aa, bb5);
      const float s6 = finish_score(dot6, aa, bb6);
      const float s7 = finish_score(dot7, aa, bb7);

      if (scores) {
        if (static_cast<size_t>(offset_idx) < score_capacity) scores[offset_idx] = s0;
        if (static_cast<size_t>(offset_idx + 1) < score_capacity) scores[offset_idx + 1] = s1;
        if (static_cast<size_t>(offset_idx + 2) < score_capacity) scores[offset_idx + 2] = s2;
        if (static_cast<size_t>(offset_idx + 3) < score_capacity) scores[offset_idx + 3] = s3;
        if (static_cast<size_t>(offset_idx + 4) < score_capacity) scores[offset_idx + 4] = s4;
        if (static_cast<size_t>(offset_idx + 5) < score_capacity) scores[offset_idx + 5] = s5;
        if (static_cast<size_t>(offset_idx + 6) < score_capacity) scores[offset_idx + 6] = s6;
        if (static_cast<size_t>(offset_idx + 7) < score_capacity) scores[offset_idx + 7] = s7;
      }

      if (s0 > result.best_score) { result.best_score = s0; result.best_offset = off0; }
      if (s1 > result.best_score) { result.best_score = s1; result.best_offset = off1; }
      if (s2 > result.best_score) { result.best_score = s2; result.best_offset = off2; }
      if (s3 > result.best_score) { result.best_score = s3; result.best_offset = off3; }
      if (s4 > result.best_score) { result.best_score = s4; result.best_offset = off4; }
      if (s5 > result.best_score) { result.best_score = s5; result.best_offset = off5; }
      if (s6 > result.best_score) { result.best_score = s6; result.best_offset = off6; }
      if (s7 > result.best_score) { result.best_score = s7; result.best_offset = off7; }

      result.offsets += 8;
      result.sample_pairs += static_cast<uint64_t>(8) * window;
    }
  }

  for (; offset_idx + 3 < total_candidates; offset_idx += 4) {
    const int off0 = -radius + offset_idx;
    const int off1 = off0 + 1;
    const int off2 = off0 + 2;
    const int off3 = off0 + 3;

    const float *__restrict b0 = b_ptr + offset_idx;

    float dot0 = 0.0f, dot1 = 0.0f, dot2 = 0.0f, dot3 = 0.0f;
    float bb0  = 0.0f, bb1  = 0.0f, bb2  = 0.0f, bb3  = 0.0f;

    float v0 = b0[0];
    float v1 = b0[1];
    float v2 = b0[2];

    int i = 0;
    for (; i + 1 < window; i += 2) {
      const float a0 = a_ptr[i];
      const float v3 = b0[i + 3];

      dot0 = fma_acc(a0, v0, dot0); bb0 = fma_acc(v0, v0, bb0);
      dot1 = fma_acc(a0, v1, dot1); bb1 = fma_acc(v1, v1, bb1);
      dot2 = fma_acc(a0, v2, dot2); bb2 = fma_acc(v2, v2, bb2);
      dot3 = fma_acc(a0, v3, dot3); bb3 = fma_acc(v3, v3, bb3);

      const float a1 = a_ptr[i + 1];
      const float v4 = b0[i + 4];

      dot0 = fma_acc(a1, v1, dot0); bb0 = fma_acc(v1, v1, bb0);
      dot1 = fma_acc(a1, v2, dot1); bb1 = fma_acc(v2, v2, bb1);
      dot2 = fma_acc(a1, v3, dot2); bb2 = fma_acc(v3, v3, bb2);
      dot3 = fma_acc(a1, v4, dot3); bb3 = fma_acc(v4, v4, bb3);

      v0 = v2;
      v1 = v3;
      v2 = v4;
    }
    for (; i < window; ++i) {
      const float a_val = a_ptr[i];
      const float v3 = b0[i + 3];

      dot0 = fma_acc(a_val, v0, dot0); bb0 = fma_acc(v0, v0, bb0);
      dot1 = fma_acc(a_val, v1, dot1); bb1 = fma_acc(v1, v1, bb1);
      dot2 = fma_acc(a_val, v2, dot2); bb2 = fma_acc(v2, v2, bb2);
      dot3 = fma_acc(a_val, v3, dot3); bb3 = fma_acc(v3, v3, bb3);

      v0 = v1;
      v1 = v2;
      v2 = v3;
    }

    const float s0 = finish_score(dot0, aa, bb0);
    const float s1 = finish_score(dot1, aa, bb1);
    const float s2 = finish_score(dot2, aa, bb2);
    const float s3 = finish_score(dot3, aa, bb3);

    if (scores) {
      if (static_cast<size_t>(offset_idx) < score_capacity) scores[offset_idx] = s0;
      if (static_cast<size_t>(offset_idx + 1) < score_capacity) scores[offset_idx + 1] = s1;
      if (static_cast<size_t>(offset_idx + 2) < score_capacity) scores[offset_idx + 2] = s2;
      if (static_cast<size_t>(offset_idx + 3) < score_capacity) scores[offset_idx + 3] = s3;
    }

    if (s0 > result.best_score) { result.best_score = s0; result.best_offset = off0; }
    if (s1 > result.best_score) { result.best_score = s1; result.best_offset = off1; }
    if (s2 > result.best_score) { result.best_score = s2; result.best_offset = off2; }
    if (s3 > result.best_score) { result.best_score = s3; result.best_offset = off3; }

    result.offsets += 4;
    result.sample_pairs += static_cast<uint64_t>(4) * window;
  }

  for (; offset_idx < total_candidates; ++offset_idx) {
    const int off = -radius + offset_idx;
    const float *__restrict b = b_ptr + offset_idx;
    float dot = 0.0f, bb = 0.0f;
    for (int i = 0; i < window; ++i) {
      const float a_val = a_ptr[i];
      const float b_val = b[i];
      dot += a_val * b_val;
      bb += b_val * b_val;
    }
    const float s = finish_score(dot, aa, bb);
    if (scores && static_cast<size_t>(offset_idx) < score_capacity)
      scores[offset_idx] = s;
    if (s > result.best_score) {
      result.best_score = s;
      result.best_offset = off;
    }
    ++result.offsets;
    result.sample_pairs += static_cast<uint64_t>(window);
  }

  return result;
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
  case PitchMarkNccVariant::ContiguousMulti4:
    return "PITCH_MARK_NCC_CONTIGUOUS_MULTI4";
  case PitchMarkNccVariant::ContiguousMulti8:
    return "PITCH_MARK_NCC_CONTIGUOUS_MULTI8";
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

  if (variant == PitchMarkNccVariant::ContiguousMulti4) {
    return search_contiguous_multi<4>(
        ring, ring_size, previous_mark, predicted_mark, radius, window,
        scratch, scratch_capacity, scores, score_capacity);
  }
  if (variant == PitchMarkNccVariant::ContiguousMulti8) {
    return search_contiguous_multi<8>(
        ring, ring_size, previous_mark, predicted_mark, radius, window,
        scratch, scratch_capacity, scores, score_capacity);
  }

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
