#include "lpc.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

int main() {
  constexpr size_t kIterations = 10000;
  constexpr size_t kOrder = 16;
  constexpr size_t kWindowSize = 1024;
  constexpr size_t kBlockSize = 64;

  std::vector<float> speech_frame(kWindowSize);
  for (size_t i = 0; i < kWindowSize; ++i) {
    speech_frame[i] = 0.3f * std::sin(0.05f * i) + 0.1f * std::sin(0.12f * i);
  }

  // 1. Benchmark LPC Analysis (Levinson-Durbin + Autocorrelation)
  SharedLpcModel model{};
  std::vector<double> lpc_solve_times;
  lpc_solve_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    SharedLpcAnalysis::solve(speech_frame.data(), kWindowSize, kOrder, 0.97f, &model);
    auto t1 = std::chrono::steady_clock::now();
    lpc_solve_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  // 2. Benchmark Warping + Bandwidth Expansion
  std::array<float, kOrder + 1> warped{};
  const float lambda = SharedLpcAnalysis::lambda_from_semitones(7.0f);
  std::vector<double> warp_times;
  warp_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    SharedLpcAnalysis::warp_polynomial(model.coefficients.data(), kOrder, lambda, 0.985f, warped.data());
    auto t1 = std::chrono::steady_clock::now();
    warp_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  // 3. Benchmark All-pole synthesis loop (per block of 64 samples)
  std::array<float, kOrder> state{};
  std::vector<float> input_pulses(kBlockSize, 0.1f);
  std::vector<float> restored_out(kBlockSize, 0.0f);
  std::vector<double> synth_block_times;
  synth_block_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kBlockSize; ++i) {
      float restored = input_pulses[i];
      for (size_t j = 1; j <= kOrder; ++j) {
        restored -= warped[j] * state[j - 1];
      }
      for (size_t j = kOrder - 1; j > 0; --j) {
        state[j] = state[j - 1] * 0.999f;
      }
      state[0] = restored;
      restored_out[i] = restored;
    }
    auto t1 = std::chrono::steady_clock::now();
    synth_block_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  // 4. Benchmark std::tanh vs Rational Softclip
  std::vector<float> tanh_in(kBlockSize);
  for (size_t i = 0; i < kBlockSize; ++i) tanh_in[i] = 0.05f * i;
  std::vector<float> tanh_out(kBlockSize);
  std::vector<double> tanh_block_times;
  tanh_block_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kBlockSize; ++i) {
      tanh_out[i] = 2.0f * std::tanh(tanh_in[i] * 0.5f);
    }
    auto t1 = std::chrono::steady_clock::now();
    tanh_block_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  std::vector<float> softclip_out(kBlockSize);
  std::vector<double> softclip_block_times;
  softclip_block_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kBlockSize; ++i) {
      float v = tanh_in[i];
      if (std::fabs(v) > 1.0f) {
        const float ax = std::fabs(v);
        const float excess = ax - 1.0f;
        v = std::copysign(1.0f + excess / (1.0f + 0.5f * excess), v);
      }
      softclip_out[i] = v;
    }
    auto t1 = std::chrono::steady_clock::now();
    softclip_block_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  // 5. Benchmark Gain matching (Multistage RMS vs Old 1-pole)
  float lpc_energy = 0.1f, psola_energy = 0.1f;
  std::vector<double> gain_block_times;
  gain_block_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kBlockSize; ++i) {
      const float r2 = tanh_out[i] * tanh_out[i];
      const float y2 = input_pulses[i] * input_pulses[i];
      lpc_energy += 0.005f * (r2 - lpc_energy);
      psola_energy += 0.005f * (y2 - psola_energy);
      float g_lpc = 1.0f;
      if (lpc_energy > 1e-8f && psola_energy > 1e-8f) {
        g_lpc = std::clamp(std::sqrt(psola_energy / lpc_energy), 0.5f, 2.0f);
      }
      tanh_out[i] *= g_lpc;
    }
    auto t1 = std::chrono::steady_clock::now();
    gain_block_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  float fast_lpc = 0.1f, fast_psola = 0.1f, slow_target = 1.0f, smoothed_g = 1.0f;
  std::vector<double> multi_gain_block_times;
  multi_gain_block_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kBlockSize; ++i) {
      const float r2 = softclip_out[i] * softclip_out[i];
      const float y2 = input_pulses[i] * input_pulses[i];
      fast_lpc += 0.0035f * (r2 - fast_lpc);
      fast_psola += 0.0035f * (y2 - fast_psola);
      if (fast_lpc > 1e-8f && fast_psola > 1e-8f) {
        const float instant_ratio = std::sqrt(fast_psola / fast_lpc);
        const float target_g = std::clamp(instant_ratio, 0.4f, 2.5f);
        const float rate = (target_g > slow_target) ? 0.0008f : 0.0002f;
        slow_target += rate * (target_g - slow_target);
      }
      const float delta = std::clamp(slow_target - smoothed_g, -0.0005f, 0.0005f);
      smoothed_g += delta;
      softclip_out[i] *= smoothed_g;
    }
    auto t1 = std::chrono::steady_clock::now();
    multi_gain_block_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  // 6. Benchmark Normalization per hop
  std::vector<double> norm_strategy_c_times;
  norm_strategy_c_times.reserve(kIterations);
  for (size_t iter = 0; iter < kIterations; ++iter) {
    auto t0 = std::chrono::steady_clock::now();
    SharedLpcAnalysis::compute_gain_normalization(model.coefficients.data(), warped.data(),
                                                  kOrder, FormantNormalizationStrategy::StrategyC_IntegratedSpectral);
    auto t1 = std::chrono::steady_clock::now();
    norm_strategy_c_times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }

  auto stats = [](std::vector<double> &v) {
    std::sort(v.begin(), v.end());
    double mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double med = v[v.size() / 2];
    double p95 = v[static_cast<size_t>(v.size() * 0.95)];
    double p99 = v[static_cast<size_t>(v.size() * 0.99)];
    double max_v = v.back();
    return std::make_tuple(mean, med, p95, p99, max_v);
  };

  auto print_s = [](const char *name, std::tuple<double, double, double, double, double> s, double div = 1.0) {
    std::printf("%-26s | Mean: %7.1f ns | Med: %7.1f ns | P95: %7.1f ns | P99: %7.1f ns | Max: %7.1f ns\n",
                name, std::get<0>(s) / div, std::get<1>(s) / div,
                std::get<2>(s) / div, std::get<3>(s) / div, std::get<4>(s) / div);
  };

  std::printf("=== FORMANT DSP BENCHMARK (Host x86_64, 10000 iterations) ===\n");
  print_s("LPC Solve (Order 16)", stats(lpc_solve_times));
  print_s("Warp + BW Exp (Hop)", stats(warp_times));
  print_s("Norm Strat C (Hop)", stats(norm_strategy_c_times));
  print_s("All-Pole Filter (Block)", stats(synth_block_times));
  print_s("All-Pole Filter (Sample)", stats(synth_block_times), 64.0);
  print_s("Old Tanh (Block)", stats(tanh_block_times));
  print_s("Old Tanh (Sample)", stats(tanh_block_times), 64.0);
  print_s("New Rational Softclip (Block)", stats(softclip_block_times));
  print_s("New Rational Softclip (Sample)", stats(softclip_block_times), 64.0);
  print_s("Old Gain Matching (Block)", stats(gain_block_times));
  print_s("New Multistage Gain (Block)", stats(multi_gain_block_times));

  return 0;
}
