#include "decimator.h"
#include <cmath>

bool FirDecimator::init(float input_rate, float output_rate) {
  if (!std::isfinite(input_rate) || !std::isfinite(output_rate) ||
      output_rate <= 0 || input_rate < output_rate)
    return false;
  const float ratio = input_rate / output_rate;
  factor_ = static_cast<uint32_t>(std::lround(ratio));
  if (factor_ < 1 || factor_ > 4 || std::fabs(ratio - factor_) > 1e-4f)
    return false;
  // Windowed-sinc LPF: cutoff 0.45 * output rate (5.4 kHz for 12 kHz),
  // transition approximately 2.5 kHz, 31 taps, 15 input-sample group delay.
  const float fc = .45f / factor_;
  float sum = 0;
  for (size_t i = 0; i < kTaps; ++i) {
    const float m = static_cast<float>(i) - (kTaps - 1) * .5f;
    const float sinc = std::fabs(m) < 1e-7f
                           ? 2 * fc
                           : std::sin(2 * 3.14159265358979323846f * fc * m) /
                                 (3.14159265358979323846f * m);
    const float window =
        .54f - .46f * std::cos(2 * 3.14159265358979323846f * i / (kTaps - 1));
    coefficients_[i] = sinc * window;
    sum += coefficients_[i];
  }
  for (float &v : coefficients_)
    v /= sum;
  reset();
  return true;
}
void FirDecimator::reset() {
  history_.fill(0);
  write_ = phase_ = 0;
}
bool FirDecimator::process(float input, float &output) {
  history_[write_] = std::isfinite(input) ? input : 0;
  write_ = (write_ + 1) % kTaps;
  if (++phase_ < factor_)
    return false;
  phase_ = 0;
  float sum = 0;
  size_t p = write_;
  for (size_t i = 0; i < kTaps; ++i) {
    p = p ? p - 1 : kTaps - 1;
    sum += coefficients_[i] * history_[p];
  }
  output = sum;
  return true;
}
