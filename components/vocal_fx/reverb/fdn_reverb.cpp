#include "fdn_reverb.h"
#include "profiling.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif

// B4D.2 link budget: the diagnostic profile counters live in PSRAM (allocated
// on enable) and the pointer lives at file scope, so the engine .bss does not
// grow. This is a single-instance diagnostic; production never allocates it.
static FdnReverb::ReverbProfile *s_reverb_profile = nullptr;

void FdnReverb::hadamard8(float *x) {
  for (int step = 1; step < 8; step *= 2)
    for (int i = 0; i < 8; i += 2 * step)
      for (int j = 0; j < step; j++) {
        float a = x[i + j], b = x[i + j + step];
        x[i + j] = a + b;
        x[i + j + step] = a - b;
      }
  constexpr float n = .3535533905932738f;
  for (int i = 0; i < 8; i++)
    x[i] *= n;
}
bool FdnReverb::init(float sr) {
  if (!std::isfinite(sr) || sr <= 0.0f)
    return false;
  sr_ = sr; /* Pairwise-incommensurate millisecond values spread modes and avoid
               a common audible period. */
  const float ms[8] = {29.7f, 32.9f, 36.1f, 39.7f, 43.3f, 47.9f, 52.1f, 58.3f};
  size_t sizes[8];
  size_t total = 0;
  for (int i = 0; i < 8; i++) {
    const size_t delay_samples = (size_t)(sr * ms[i] * .001f);
    if (delay_samples == 0)
      return false;
    sizes[i] = delay_samples;
    total += (delay_samples + 15u) & ~static_cast<size_t>(15u); // 64-byte slices
  }
  total += 16; // alignment slack
  pool_ = std::unique_ptr<float[]>(new (std::nothrow) float[total]());
  if (!pool_)
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(pool_.get());
  const size_t misalign = (64u - (base & 63u)) & 63u;
  float *p = pool_.get() + misalign / sizeof(float);
  for (int i = 0; i < 8; i++) {
    lines_[i].b = p;
    lines_[i].size = sizes[i];
    lines_[i].pos = 0;
    lines_[i].damping_state = 0.0f;
    p += (sizes[i] + 15u) & ~static_cast<size_t>(15u);
  }
  if (!diffuser_.init(sr))
    return false;
  wet_.init(.18f, sr, 30);
  set_rt60(2);
  set_damping(.45f);
  return true;
}
void FdnReverb::reset() {
  for (auto &x : lines_) {
    std::fill_n(x.b, x.size, 0.0f);
    x.pos = 0;
    x.damping_state = 0;
  }
  diffuser_.reset();
}
void FdnReverb::set_rt60(float s) {
  s = std::clamp(s, .15f, 20.0f);
  for (auto &x : lines_)
    x.feedback_gain = std::pow(10.0f, -3.0f * ((float)x.size / sr_) / s);
}
void FdnReverb::set_damping(float n) {
  n = std::clamp(n, 0.0f, 1.0f);
  float hz = 18000.0f * std::pow(500.0f / 18000.0f, n);
  damping_coefficient_ = std::exp(-2 * 3.14159265f * hz / sr_);
}
void FdnReverb::set_wet(float w) { wet_.set_target(std::clamp(w, 0.0f, 1.0f)); }

// Reference per-sample path. Retained verbatim (modulo the exact wrap-advance)
// so host tests and any non-block caller keep bit-identical output.
void FdnReverb::process(float in, float &l, float &r) {
  float x[8];
  for (int i = 0; i < 8; i++) {
    auto &q = lines_[i];
    float d = q.b[q.pos];
    q.damping_state =
        (1 - damping_coefficient_) * d + damping_coefficient_ * q.damping_state;
    x[i] = q.damping_state;
  }
  constexpr float scale = .25f;
  // Output taps are the pre-Hadamard values; computing them here is identical
  // to the former taps[] copy but avoids the copy.
  l = (x[0] + x[1] - x[2] + x[3] - x[4] - x[5] + x[6] - x[7]) * scale;
  r = (x[0] - x[1] + x[2] + x[3] - x[4] + x[5] - x[6] - x[7]) * scale;
  hadamard8(x);
  float feed = diffuser_.process(in) * .35355339f;
  for (int i = 0; i < 8; i++) {
    auto &q = lines_[i];
    q.b[q.pos] = feed + x[i] * q.feedback_gain;
    if (++q.pos == q.size) q.pos = 0;
  }
  float w = wet_.next();
  l *= w;
  r *= w;
}

// Production block path. Same arithmetic and per-sample evaluation order as
// process(); per-line state is cached in locals for the whole block and the
// ring indices advance with one wrap branch (no runtime modulo).
void FdnReverb::process_block(const float *in, float *l, float *r, size_t n) {
  const float dc = damping_coefficient_;
  const float omdc = 1.0f - dc;
  constexpr float feed_scale = .35355339f;
  constexpr float scale = .25f;

  float *buf[8];
  size_t pos[8], sz[8];
  float ds[8], fb[8];
  for (int i = 0; i < 8; i++) {
    buf[i] = lines_[i].b;
    pos[i] = lines_[i].pos;
    sz[i] = lines_[i].size;
    ds[i] = lines_[i].damping_state;
    fb[i] = lines_[i].feedback_gain;
  }

  if (!s_reverb_profile) {
    constexpr float hn = .3535533905932738f;
    for (size_t s = 0; s < n; ++s) {
      // Scalar registers for the eight line taps: identical arithmetic to
      // hadamard8()/the array path, but no stack-array round trips.
      float x0 = buf[0][pos[0]];
      ds[0] = omdc * x0 + dc * ds[0];
      x0 = ds[0];
      float x1 = buf[1][pos[1]];
      ds[1] = omdc * x1 + dc * ds[1];
      x1 = ds[1];
      float x2 = buf[2][pos[2]];
      ds[2] = omdc * x2 + dc * ds[2];
      x2 = ds[2];
      float x3 = buf[3][pos[3]];
      ds[3] = omdc * x3 + dc * ds[3];
      x3 = ds[3];
      float x4 = buf[4][pos[4]];
      ds[4] = omdc * x4 + dc * ds[4];
      x4 = ds[4];
      float x5 = buf[5][pos[5]];
      ds[5] = omdc * x5 + dc * ds[5];
      x5 = ds[5];
      float x6 = buf[6][pos[6]];
      ds[6] = omdc * x6 + dc * ds[6];
      x6 = ds[6];
      float x7 = buf[7][pos[7]];
      ds[7] = omdc * x7 + dc * ds[7];
      x7 = ds[7];

      const float lo =
          (x0 + x1 - x2 + x3 - x4 - x5 + x6 - x7) * scale;
      const float ro =
          (x0 - x1 + x2 + x3 - x4 + x5 - x6 - x7) * scale;

      // In-place 8-point Hadamard (same butterfly order as hadamard8).
      const float a0 = x0 + x1, a1 = x0 - x1, a2 = x2 + x3, a3 = x2 - x3;
      const float a4 = x4 + x5, a5 = x4 - x5, a6 = x6 + x7, a7 = x6 - x7;
      const float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
      const float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
      x0 = (b0 + b4) * hn;
      x4 = (b0 - b4) * hn;
      x1 = (b1 + b5) * hn;
      x5 = (b1 - b5) * hn;
      x2 = (b2 + b6) * hn;
      x6 = (b2 - b6) * hn;
      x3 = (b3 + b7) * hn;
      x7 = (b3 - b7) * hn;

      const float feed = diffuser_.process(in[s]) * feed_scale;
      buf[0][pos[0]] = feed + x0 * fb[0];
      if (++pos[0] == sz[0]) pos[0] = 0;
      buf[1][pos[1]] = feed + x1 * fb[1];
      if (++pos[1] == sz[1]) pos[1] = 0;
      buf[2][pos[2]] = feed + x2 * fb[2];
      if (++pos[2] == sz[2]) pos[2] = 0;
      buf[3][pos[3]] = feed + x3 * fb[3];
      if (++pos[3] == sz[3]) pos[3] = 0;
      buf[4][pos[4]] = feed + x4 * fb[4];
      if (++pos[4] == sz[4]) pos[4] = 0;
      buf[5][pos[5]] = feed + x5 * fb[5];
      if (++pos[5] == sz[5]) pos[5] = 0;
      buf[6][pos[6]] = feed + x6 * fb[6];
      if (++pos[6] == sz[6]) pos[6] = 0;
      buf[7][pos[7]] = feed + x7 * fb[7];
      if (++pos[7] == sz[7]) pos[7] = 0;

      const float w = wet_.next();
      l[s] = lo * w;
      r[s] = ro * w;
    }
  } else {
    // Diagnostic path: identical arithmetic, coarse subsection accounting.
    for (size_t s = 0; s < n; ++s) {
      float x[8];
      const uint32_t t0 = Profiler::now_cycles();
      for (int i = 0; i < 8; i++) {
        const float d = buf[i][pos[i]];
        ds[i] = omdc * d + dc * ds[i];
        x[i] = ds[i];
      }
      const uint32_t t1 = Profiler::now_cycles();
      float lo = (x[0] + x[1] - x[2] + x[3] - x[4] - x[5] + x[6] - x[7]) * scale;
      float ro = (x[0] - x[1] + x[2] + x[3] - x[4] + x[5] - x[6] - x[7]) * scale;
      const uint32_t t2 = Profiler::now_cycles();
      hadamard8(x);
      const uint32_t t3 = Profiler::now_cycles();
      const float feed = diffuser_.process(in[s]) * feed_scale;
      const uint32_t t4 = Profiler::now_cycles();
      for (int i = 0; i < 8; i++) {
        buf[i][pos[i]] = feed + x[i] * fb[i];
        if (++pos[i] == sz[i]) pos[i] = 0;
      }
      const uint32_t t5 = Profiler::now_cycles();
      const float w = wet_.next();
      l[s] = lo * w;
      r[s] = ro * w;
      const uint32_t t6 = Profiler::now_cycles();
      s_reverb_profile->read_damping_cycles += t1 - t0;
      s_reverb_profile->mix_cycles += t2 - t1;
      s_reverb_profile->hadamard_cycles += t3 - t2;
      s_reverb_profile->diffuser_cycles += t4 - t3;
      s_reverb_profile->write_index_cycles += t5 - t4;
      s_reverb_profile->wet_mix_cycles += t6 - t5;
      s_reverb_profile->total_cycles += t6 - t0;
      s_reverb_profile->samples++;
    }
    s_reverb_profile->blocks++;
  }

  for (int i = 0; i < 8; i++) {
    lines_[i].pos = pos[i];
    lines_[i].damping_state = ds[i];
  }
}

void FdnReverb::set_profile_enabled(bool enabled) {
  if (!enabled) {
    if (s_reverb_profile) {
#ifdef ESP_PLATFORM
      heap_caps_free(s_reverb_profile);
#else
      std::free(s_reverb_profile);
#endif
      s_reverb_profile = nullptr;
    }
    return;
  }
  if (!s_reverb_profile) {
#ifdef ESP_PLATFORM
    s_reverb_profile = static_cast<ReverbProfile *>(
        heap_caps_calloc(1, sizeof(ReverbProfile),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    s_reverb_profile = static_cast<ReverbProfile *>(std::calloc(1, sizeof(ReverbProfile)));
#endif
  }
  if (s_reverb_profile) *s_reverb_profile = ReverbProfile{};
}

void FdnReverb::reset_profile() {
  if (s_reverb_profile) *s_reverb_profile = ReverbProfile{};
}

bool FdnReverb::profile_enabled() const { return s_reverb_profile != nullptr; }

FdnReverb::ReverbProfile FdnReverb::profile() const {
  return s_reverb_profile ? *s_reverb_profile : ReverbProfile{};
}

bool FdnReverb::line_in_psram(size_t i) const {
  if (i >= 8 || !lines_[i].b) return false;
#ifdef ESP_PLATFORM
  return esp_ptr_external_ram(lines_[i].b);
#else
  return false;
#endif
}
bool FdnReverb::diffuser_in_psram(size_t i) const {
#ifdef ESP_PLATFORM
  const void *p = diffuser_.stage_ptr(i);
  return p ? esp_ptr_external_ram(p) : false;
#else
  (void)i;
  return false;
#endif
}

size_t FdnReverb::memory_bytes() const {
  size_t n = diffuser_.memory_bytes();
  for (auto &q : lines_)
    n += q.size * sizeof(float);
  return n;
}
