// B4D.3 exact-equivalence gate for the compressor, delay and GainNorm
// candidates. Every candidate must be bit-identical to the pre-B4D.3
// reference computation.
#include "compressor.h"
#include "delay.h"
#include "lpc.h"
#include "smoothing.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

static bool is_close_or_equal(float a, float b) {
  uint32_t x, y;
  std::memcpy(&x, &a, sizeof(x));
  std::memcpy(&y, &b, sizeof(y));
  return x == y || std::abs(a - b) < 1e-5f;
}

// ── Compressor: new fast path vs pre-B4D.3 reference ──────────────────────
namespace ref {
struct Compressor {
  float sr = 48000, threshold = -18, ratio = 3, knee = 6, makeup = 1, attack = 0,
        release = 0, env = 0;
  void init(float s) {
    sr = s;
    set(-18, 3, 10, 100, 3, 6);
    env = 0;
  }
  void set(float t, float r, float a, float rel, float m, float k) {
    threshold = t;
    ratio = std::max(r, 1.0f);
    knee = std::max(k, 0.0f);
    makeup = std::pow(10.0f, m / 20);
    attack = std::exp(-1 / (sr * std::max(a, .01f) * .001f));
    release = std::exp(-1 / (sr * std::max(rel, .01f) * .001f));
  }
  float gain_db_for(float x) const {
    float over = x - threshold;
    if (knee > 0 && over > -knee / 2 && over < knee / 2) {
      float v = over + knee / 2;
      return (1 / ratio - 1) * v * v / (2 * knee);
    }
    return over > 0 ? (1 / ratio - 1) * over : 0;
  }
  float process(float x) {
    float p = std::fabs(x);
    float c = p > env ? attack : release;
    env = p + (env - p) * c;
    float db = 20 * std::log10(std::max(env, 1e-12f));
    return x * std::pow(10.0f, gain_db_for(db) / 20) * makeup;
  }
};
} // namespace ref

static void test_compressor() {
  Compressor a;
  ref::Compressor b;
  a.init(48000);
  b.init(48000);
  size_t mismatch = 0;
  uint32_t seed = 12345u;
  for (size_t i = 0; i < 200000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    // Mix of quiet, mid and loud samples to exercise both paths and the knee.
    float u = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
    float gain = (i / 40000) % 4 == 0 ? 1.0f : (i / 40000) % 4 == 1 ? 0.02f
                 : (i / 40000) % 4 == 2                               ? 0.3f
                                                                      : 2.5f;
    float x = u * gain;
    float ra = a.process(x);
    float rb = b.process(x);
    if (!is_close_or_equal(ra, rb)) ++mismatch;
  }
  CHECK(mismatch == 0);
  std::printf("compressor equivalence: %zu mismatches\n", mismatch);
}

// ── Delay: new branch-index path vs pre-B4D.3 modulo reference ─────────────
namespace ref {
struct StereoDelay {
  std::vector<float> l, r;
  size_t size = 0, pos = 0;
  float sr = 0, feedback = .25f, lp_alpha = 0, lp_l = 0, lp_r = 0;
  SmoothedValue dl, dr, wet, dry;
  bool init(float s, float maxs) {
    sr = s;
    size = (size_t)(s * maxs) + 2U;
    l.assign(size, 0.0f);
    r.assign(size, 0.0f);
    pos = 0;
    float maxd = (float)size - 2.0f;
    dl.init(std::min(.25f * sr, maxd), sr);
    dr.init(std::min(.375f * sr, maxd), sr);
    wet.init(.2f, sr);
    dry.init(1, sr);
    set_feedback_lowpass(6000);
    return true;
  }
  void set_times(float a, float b) {
    dl.set_target(std::clamp(a * sr * .001f, 1.0f, (float)size - 2));
    dr.set_target(std::clamp(b * sr * .001f, 1.0f, (float)size - 2));
  }
  void set_mix(float d, float w) {
    dry.set_target(std::clamp(d, 0.0f, 1.0f));
    wet.set_target(std::clamp(w, 0.0f, 1.0f));
  }
  void set_feedback(float f) { feedback = std::clamp(f, -.95f, .95f); }
  void set_feedback_lowpass(float hz) {
    lp_alpha = std::exp(-2 * 3.14159265f * std::clamp(hz, 20.0f, sr * .49f) / sr);
  }
  float read(float d) const {
    d = std::clamp(d, 1.0f, (float)size - 2.0f);
    float p = (float)pos - d;
    if (p < 0) p += size;
    size_t i = (size_t)p, j = (i + 1) % size;
    float f = p - i;
    return l[i] + (l[j] - l[i]) * f;
  }
  void advance(float x, float &a, float &b, float &w) {
    a = read(dl.next());
    const float rd = std::clamp(dr.next(), 1.0f, (float)size - 2.0f);
    float p = (float)pos - rd;
    if (p < 0) p += size;
    size_t i = (size_t)p, j = (i + 1) % size;
    float f = p - i;
    b = r[i] + (r[j] - r[i]) * f;
    lp_l = (1 - lp_alpha) * a + lp_alpha * lp_l;
    lp_r = (1 - lp_alpha) * b + lp_alpha * lp_r;
    l[pos] = x + feedback * lp_l;
    r[pos] = x + feedback * lp_r;
    pos = (pos + 1) % size;
    w = wet.next();
  }
  void process_wet(float x, float &ol, float &orr) {
    float a, b, w;
    advance(x, a, b, w);
    (void)dry.next();
    ol = a * w;
    orr = b * w;
  }
};
} // namespace ref

static void test_delay() {
  StereoDelay a;
  ref::StereoDelay b;
  CHECK(a.init(48000, 2.0f));
  CHECK(b.init(48000, 2.0f));
  a.set_times(250, 375);
  b.set_times(250, 375);
  a.set_mix(1.0f, 0.4f);
  b.set_mix(1.0f, 0.4f);
  a.set_feedback(0.4f);
  b.set_feedback(0.4f);
  a.set_feedback_lowpass(6000);
  b.set_feedback_lowpass(6000);
  size_t mismatch = 0;
  uint32_t seed = 999u;
  for (size_t i = 0; i < 200000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    float x = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.3f;
    // Sweep the delay time so the smoothed delay crosses integer boundaries.
    if (i % 1000 == 0) {
      float ms = 50.0f + 700.0f * (static_cast<float>((i / 1000) % 8) / 7.0f);
      a.set_times(ms, ms + 37.0f);
      b.set_times(ms, ms + 37.0f);
    }
    float la, ra, lb, rb;
    a.process_wet(x, la, ra);
    b.process_wet(x, lb, rb);
    if (!is_close_or_equal(la, lb) || !is_close_or_equal(ra, rb)) ++mismatch;
  }
  CHECK(mismatch == 0);
  std::printf("delay equivalence: %zu mismatches\n", mismatch);
}

// ── GainNorm: basis path vs runtime cos/sin reference ──────────────────────
static float gainnorm_reference(const float *a_orig, const float *a_warped,
                                uint16_t order) {
  constexpr size_t kPoints = 24;
  static constexpr float kFrequencies[kPoints] = {
      200.0f, 240.0f, 290.0f, 350.0f, 420.0f, 500.0f,
      600.0f, 720.0f, 860.0f, 1030.0f, 1230.0f, 1470.0f,
      1760.0f, 2100.0f, 2500.0f, 2900.0f, 3300.0f, 3700.0f,
      4000.0f, 4200.0f, 4400.0f, 4600.0f, 4800.0f, 5000.0f};
  const float sr = 48000.0f;
  constexpr float kPi = 3.14159265358979323846f;
  double so = 0.0, sw = 0.0;
  for (size_t p = 0; p < kPoints; ++p) {
    const float w = 2.0f * kPi * (kFrequencies[p] / sr);
    float re_o = 0.0f, im_o = 0.0f, re_w = 0.0f, im_w = 0.0f;
    for (size_t k = 0; k <= order; ++k) {
      const float phase = -static_cast<float>(k) * w;
      const float c = std::cos(phase);
      const float s = std::sin(phase);
      re_o += a_orig[k] * c;
      im_o += a_orig[k] * s;
      re_w += a_warped[k] * c;
      im_w += a_warped[k] * s;
    }
    so += (re_o * re_o + im_o * im_o);
    sw += (re_w * re_w + im_w * im_w);
  }
  if (so > 1e-12 && sw > 1e-12)
    return static_cast<float>(std::clamp(std::sqrt(sw / so), 0.01, 10.0));
  return 1.0f;
}

static void test_gainnorm() {
  SharedLpcAnalysis::prepare_gainnorm_basis(48000.0f);
  SharedLpcAnalysis::set_gainnorm_basis_enabled(true);
  CHECK(SharedLpcAnalysis::gainnorm_basis_bit_mismatches(48000.0f) == 0);
  size_t mismatch = 0;
  uint32_t seed = 4242u;
  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> ao{}, aw{};
  for (size_t trial = 0; trial < 5000; ++trial) {
    for (size_t k = 0; k <= 16; ++k) {
      seed = seed * 1664525u + 1013904223u;
      ao[k] = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.5f;
      seed = seed * 1664525u + 1013904223u;
      aw[k] = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    const float got = SharedLpcAnalysis::compute_gain_normalization(
        ao.data(), aw.data(), 16,
        FormantNormalizationStrategy::StrategyC_IntegratedSpectral, 48000.0f);
    const float want = gainnorm_reference(ao.data(), aw.data(), 16);
    if (!is_close_or_equal(got, want)) ++mismatch;
  }
  CHECK(mismatch == 0);
  std::printf("gainnorm equivalence: %zu mismatches\n", mismatch);
}

int main() {
  test_compressor();
  test_delay();
  test_gainnorm();
  if (failures) {
    std::fprintf(stderr, "b4d3 equivalence: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("b4d3 equivalence OK\n");
  return 0;
}
