#include "lpc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

struct ComparisonTotals {
  uint64_t frames = 0;
  uint64_t wrap_boundaries = 0;
  float max_coefficient_error = 0.0f;
  float max_prediction_error = 0.0f;
  float max_confidence_error = 0.0f;
  bool bit_identical = true;
  bool timestamps_identical = true;
  bool frame_counts_identical = true;
};

uint32_t float_bits(float value) {
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

class SlidingLpcReference {
public:
  explicit SlidingLpcReference(LpcConfig config) : config_(config) {}

  bool push(float value, const PitchResult &pitch, SharedLpcModel *model) {
    const uint64_t position = input_position_++;
    if (fill_ < config_.window_size)
      frame_[fill_++] = std::isfinite(value) ? value : 0.0f;
    else {
      std::move(frame_.begin() + 1, frame_.begin() + config_.window_size,
                frame_.begin());
      frame_[config_.window_size - 1] = std::isfinite(value) ? value : 0.0f;
    }
    if (fill_ != config_.window_size ||
        !(++since_frame_ >= config_.hop_size || frames_ == 0))
      return false;

    since_frame_ = 0;
    SharedLpcModel result{};
    result.timestamp = position - config_.window_size / 2;
    const bool voiced = pitch.voiced && pitch.confidence > .25f;
    result.valid =
        config_.enabled && voiced &&
        SharedLpcAnalysis::solve(frame_.data(), config_.window_size,
                                 config_.order, config_.preemphasis, &result);
    result.confidence *= std::clamp(pitch.confidence, 0.0f, 1.0f);
    ++frames_;
    *model = result;
    return true;
  }

  uint64_t frames() const { return frames_; }

private:
  LpcConfig config_{};
  std::array<float, 1024> frame_{};
  size_t fill_ = 0;
  size_t since_frame_ = 0;
  uint64_t input_position_ = 0;
  uint64_t frames_ = 0;
};

void compare_model(const SharedLpcModel &reference,
                   const SharedLpcModel &ring, ComparisonTotals *totals) {
  CHECK(reference.timestamp == ring.timestamp);
  CHECK(reference.valid == ring.valid);
  CHECK(reference.order == ring.order);
  totals->timestamps_identical &= reference.timestamp == ring.timestamp;
  for (size_t i = 0; i < reference.coefficients.size(); ++i) {
    const float error =
        std::fabs(reference.coefficients[i] - ring.coefficients[i]);
    totals->max_coefficient_error =
        std::max(totals->max_coefficient_error, error);
    totals->bit_identical &=
        float_bits(reference.coefficients[i]) == float_bits(ring.coefficients[i]);
    CHECK(error <= 1e-7f);
  }
  const float prediction_error =
      std::fabs(reference.prediction_error - ring.prediction_error);
  const float confidence_error =
      std::fabs(reference.confidence - ring.confidence);
  totals->max_prediction_error =
      std::max(totals->max_prediction_error, prediction_error);
  totals->max_confidence_error =
      std::max(totals->max_confidence_error, confidence_error);
  totals->bit_identical &=
      float_bits(reference.prediction_error) == float_bits(ring.prediction_error);
  totals->bit_identical &=
      float_bits(reference.confidence) == float_bits(ring.confidence);
  CHECK(prediction_error <= 1e-7f);
  CHECK(confidence_error <= 1e-7f);
}

void run_equivalence_case(const char *name, const std::vector<float> &samples,
                          ComparisonTotals *totals) {
  LpcConfig config{};
  // B4B.4A isolates circular-vs-sliding storage. Keep its arithmetic oracle
  // explicit now that production uses the B4B.4C double-single candidate.
  config.autocorrelation =
      LpcAutocorrelationVariant::AutocorrReferenceDouble;
  SharedLpcAnalysis ring;
  CHECK(ring.init(48000.0f, config));
  SlidingLpcReference reference(config);
  PitchResult pitch{};
  pitch.voiced = true;
  pitch.confidence = .83f;
  uint64_t ring_frames = 0;

  for (float sample : samples) {
    SharedLpcModel expected{};
    const bool reference_emitted = reference.push(sample, pitch, &expected);
    ring.tap(&sample, 1);
    const size_t emitted = ring.run(1, pitch);
    CHECK(emitted == static_cast<size_t>(reference_emitted));
    if (emitted != 0) {
      SharedLpcModel actual{};
      CHECK(ring.latest_model(&actual));
      compare_model(expected, actual, totals);
      ++ring_frames;
      ++totals->frames;
    }
  }
  CHECK(ring_frames == reference.frames());
  CHECK(ring.telemetry().lpc_frames == reference.frames());
  totals->frame_counts_identical &= ring_frames == reference.frames();
  totals->wrap_boundaries += samples.size() / config.window_size;
  std::printf("LPC_RING_CASE name=%s samples=%zu frames=%llu wraps=%zu PASS\n",
              name, samples.size(),
              static_cast<unsigned long long>(ring_frames),
              samples.size() / config.window_size);
}

std::vector<float> load_vocal_fixture(size_t maximum_frames) {
  std::ifstream input("../samples/dry-acapella-lead-vocal.wav",
                      std::ios::binary);
  if (!input)
    input.open("../samples/dry-acapella-leave-this-place_95bpm.wav",
               std::ios::binary);
  if (!input)
    input.open("samples/dry-acapella-leave-this-place_95bpm.wav",
               std::ios::binary);
  if (!input)
    return {};
  std::array<char, 12> riff{};
  input.read(riff.data(), riff.size());
  if (!input || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
      std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
    return {};
  uint16_t format = 0, channels = 0, bits = 0;
  std::vector<char> audio;
  while (input && (audio.empty() || bits == 0)) {
    std::array<char, 4> id{};
    uint32_t size = 0;
    input.read(id.data(), id.size());
    input.read(reinterpret_cast<char *>(&size), sizeof(size));
    if (!input) break;
    if (std::memcmp(id.data(), "fmt ", 4) == 0) {
      std::vector<char> fmt(size);
      input.read(fmt.data(), size);
      if (size >= 16) {
        std::memcpy(&format, fmt.data(), 2);
        std::memcpy(&channels, fmt.data() + 2, 2);
        std::memcpy(&bits, fmt.data() + 14, 2);
      }
    } else if (std::memcmp(id.data(), "data", 4) == 0) {
      audio.resize(size);
      input.read(audio.data(), size);
    } else {
      input.seekg(size, std::ios::cur);
    }
    if (size & 1U) input.seekg(1, std::ios::cur);
  }
  if (format != 1 || channels == 0 || bits != 16 || audio.empty()) return {};
  const size_t frame_bytes = channels * sizeof(int16_t);
  const size_t frames = std::min(maximum_frames, audio.size() / frame_bytes);
  std::vector<float> result(frames);
  for (size_t i = 0; i < frames; ++i) {
    int16_t sample = 0;
    std::memcpy(&sample, audio.data() + i * frame_bytes, sizeof(sample));
    result[i] = static_cast<float>(sample) / 32768.0f;
  }
  return result;
}

void test_linearized_order() {
  LpcConfig config{};
  config.window_size = 16;
  config.hop_size = 5;
  config.order = 4;
  SharedLpcAnalysis ring;
  CHECK(ring.init(48000.0f, config));
  PitchResult pitch{};
  pitch.voiced = false;
  std::array<float, 21> ramp{};
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);
  for (size_t i = 0; i < 16; ++i) {
    ring.tap(&ramp[i], 1);
    ring.run(1, pitch);
  }
  const auto *linear = static_cast<const float *>(ring.linear_frame_ptr());
  for (size_t i = 0; i < 16; ++i) CHECK(linear[i] == static_cast<float>(i));
  for (size_t i = 16; i < ramp.size(); ++i) {
    ring.tap(&ramp[i], 1);
    ring.run(1, pitch);
  }
  for (size_t i = 0; i < 16; ++i)
    CHECK(linear[i] == static_cast<float>(i + 5));
  std::printf("LPC_RING_ORDER first=5 last=20 wraps=1 PASS\n");
}
} // namespace

int main() {
  constexpr float kPi = 3.14159265358979323846f;
  ComparisonTotals totals{};

  run_equivalence_case("silence", std::vector<float>(8192, 0.0f), &totals);

  std::vector<float> sine(16384);
  for (size_t i = 0; i < sine.size(); ++i)
    sine[i] = .25f * std::sin(2.0f * kPi * 220.0f * i / 48000.0f);
  run_equivalence_case("sine_220", sine, &totals);

  std::vector<float> harmonics(16384);
  for (size_t i = 0; i < harmonics.size(); ++i) {
    const float phase = 2.0f * kPi * 220.0f * i / 48000.0f;
    harmonics[i] = .25f * std::sin(phase) + .0625f * std::sin(2.0f * phase) +
                   .03125f * std::sin(3.0f * phase);
  }
  run_equivalence_case("harmonic", harmonics, &totals);

  std::vector<float> noise(1024 * 101 + 17);
  uint32_t random = 0x51A7C0DEu;
  for (float &sample : noise) {
    random = random * 1664525u + 1013904223u;
    sample = static_cast<float>(static_cast<int32_t>(random)) /
             2147483648.0f * .2f;
  }
  run_equivalence_case("deterministic_noise_101_wraps", noise, &totals);

  const auto vocal = load_vocal_fixture(32768);
  CHECK(!vocal.empty());
  if (!vocal.empty()) run_equivalence_case("vocal_wav", vocal, &totals);

  test_linearized_order();
  std::printf("B4B4A_EQUIVALENCE cases=5 frames=%llu wrap_boundaries=%llu "
              "bit_identical=%s timestamps_identical=%s "
              "frame_counts_identical=%s max_coefficient_error=%.9g "
              "max_prediction_error=%.9g max_confidence_error=%.9g %s\n",
              static_cast<unsigned long long>(totals.frames),
              static_cast<unsigned long long>(totals.wrap_boundaries),
              totals.bit_identical ? "yes" : "no",
              totals.timestamps_identical ? "yes" : "no",
              totals.frame_counts_identical ? "yes" : "no",
              totals.max_coefficient_error, totals.max_prediction_error,
              totals.max_confidence_error, failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
