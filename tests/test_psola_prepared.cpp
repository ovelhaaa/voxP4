#include "td_psola.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
bool same_bits(float a, float b) {
  return std::memcmp(&a, &b, sizeof(float)) == 0;
}

void publish_blocks(SharedLpcAnalysis &lpc, PitchResult &pitch, int blocks,
                    double &phase) {
  std::array<float, 64> input{};
  for (int b = 0; b < blocks; ++b) {
    for (float &sample : input) {
      sample = 0.3f * static_cast<float>(std::sin(phase));
      phase += 2.0 * 3.14159265358979323846 * 220.0 / 48000.0;
    }
    lpc.tap(input.data(), input.size());
    lpc.run(8, pitch);
  }
}

bool check_snapshot(const SharedLpcAnalysis &lpc,
                    std::array<SharedLpcModel, SharedLpcAnalysis::kModelCount> &models,
                    size_t &count) {
  if (!lpc.snapshot_models_once(models.data(), models.size(), &count))
    return false;
  for (size_t i = 0; i < count; ++i) {
    if (!models[i].valid || models[i].publication_generation == 0 ||
        models[i].order > VOCAL_FX_LPC_MAX_ORDER)
      return false;
    if (i && models[i - 1].publication_generation <=
                 models[i].publication_generation)
      return false;
  }
  return true;
}
} // namespace

int main() {
  SharedLpcAnalysis lpc;
  LpcConfig config{};
  if (!lpc.init(48000.0f, config)) return 1;
  PitchResult pitch{};
  pitch.voiced = true;
  pitch.confidence = 1.0f;
  double phase = 0;
  publish_blocks(lpc, pitch, 600, phase);

  std::array<SharedLpcModel, SharedLpcAnalysis::kModelCount> models{};
  size_t count = 0;
  if (!check_snapshot(lpc, models, count) || count != 16) return 2;
  PsolaPreparedWarpStore prepared{};
  const float lambda = SharedLpcAnalysis::lambda_from_semitones(3.0f);
  const float gamma = 0.985f;
  const auto strategy = FormantNormalizationStrategy::StrategyC_IntegratedSpectral;
  prepared.prepare(lpc, lambda, gamma, strategy, 48000.0f, 4, 4);
  if (prepared.stats.preparations != 4) return 3;
  prepared.prepare(lpc, lambda, gamma, strategy, 48000.0f, 4, 4);
  if (prepared.stats.preparations != 4 || prepared.stats.polls != 2) return 14;
  for (size_t i = 0; i < 4; ++i) {
    const auto *hit = prepared.find(models[i], lambda, gamma, strategy, 48000.0f);
    if (!hit) return 4;
    std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> expected{};
    if (!SharedLpcAnalysis::warp_polynomial(
            models[i].coefficients.data(), models[i].order, lambda, gamma,
            expected.data()))
      return 5;
    for (size_t c = 0; c <= models[i].order; ++c)
      if (!same_bits(expected[c], hit->warped_coefficients[c])) return 6;
    const float gain = SharedLpcAnalysis::compute_gain_normalization(
        models[i].coefficients.data(), expected.data(), models[i].order,
        strategy, 48000.0f);
    if (!same_bits(gain, hit->formant_filter_gain)) return 7;
  }
  SharedLpcModel stale = models[0];
  ++stale.publication_generation;
  if (prepared.find(stale, lambda, gamma, strategy, 48000.0f)) return 8;
  stale = models[0];
  stale.publication_generation = 0;
  if (prepared.find(stale, lambda, gamma, strategy, 48000.0f)) return 15;
  stale = models[0];
  stale.coefficients[2] = std::nextafter(stale.coefficients[2], INFINITY);
  if (prepared.find(stale, lambda, gamma, strategy, 48000.0f)) return 9;
  if (prepared.find(models[0], std::nextafter(lambda, INFINITY), gamma,
                    strategy, 48000.0f)) return 10;
  if (prepared.find(models[0], lambda, gamma, strategy, 44100.0f)) return 11;
  if (!prepared.find(models[0], lambda, gamma, strategy, 48000.0f, true) ||
      prepared.stats.useful_preparations != 1) return 16;

  // Ring wrap and concurrent publication can invalidate a snapshot, but a
  // successful bounded snapshot must always contain coherent generations.
  std::atomic<bool> done{false};
  std::thread writer([&] {
    publish_blocks(lpc, pitch, 2000, phase);
    done.store(true, std::memory_order_release);
  });
  size_t coherent = 0;
  while (!done.load(std::memory_order_acquire)) {
    if (check_snapshot(lpc, models, count)) ++coherent;
  }
  writer.join();
  if (!check_snapshot(lpc, models, count) || coherent == 0) return 12;
  for (size_t i = 0; i < count; ++i)
    if (prepared.find(models[i], lambda, gamma, strategy, 48000.0f))
      return 13;
  std::printf("PSOLA_PREPARED_TEST coherent=%zu generations=%zu result=PASS\n",
              coherent, count);
  return 0;
}
