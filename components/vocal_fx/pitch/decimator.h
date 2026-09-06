#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

class FirDecimator {
public:
  static constexpr size_t kTaps = 31;
  bool init(float input_rate, float output_rate);
  void reset();
  bool process(float input, float &output);
  uint32_t factor() const { return factor_; }
  float group_delay_input_samples() const { return (kTaps - 1) * .5f; }

private:
  std::array<float, kTaps> coefficients_{}, history_{};
  size_t write_ = 0;
  uint32_t phase_ = 0, factor_ = 0;
};
