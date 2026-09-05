#include "vocal_fx.h"
#include <chrono>
#include <cmath>
#include <cstdio>

int main() {
  VocalFxConfig config{};
  if (!vocal_fx_init(config))
    return 1;
  float input[64], left[64], right[64];
  for (int i = 0; i < 64; ++i)
    input[i] = 0.1f * std::sin(0.1f * i);
  constexpr int blocks = 100000;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < blocks; ++i)
    vocal_fx_process(input, left, right, 64);
  const auto elapsed = std::chrono::duration<double, std::micro>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  std::printf(
      "host avg %.3f us/block (%.2f%% of 1.333 ms), DSP buffers %zu bytes\n",
      elapsed / blocks, elapsed / blocks / 13.33333,
      vocal_fx_dsp_memory_bytes());
  return (!std::isfinite(left[63]) || !std::isfinite(right[63])) ? 2 : 0;
}
