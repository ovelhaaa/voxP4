// B4D.5 §17 model-lookup equivalence gate.
//
// The production model_near() was changed to read the cheap fields first and
// copy the coefficient block only when a candidate can beat the current best.
// This test drives the *actual* SharedLpcAnalysis::model_near against a
// reference that brute-forces the same published-model snapshot and reports:
//   selected model identity mismatch = 0
//   coefficient mismatch = 0
// Non-zero mismatches fail the test.
#include "lpc.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool ref_model_near(const SharedLpcModel *models, size_t n, uint64_t ts,
                    SharedLpcModel *out, uint64_t window_hop) {
  bool found = false;
  uint64_t best = UINT64_MAX;
  for (size_t k = 0; k < n; ++k) {
    const SharedLpcModel &m = models[k];
    const uint64_t d =
        ts > m.timestamp ? ts - m.timestamp : m.timestamp - ts;
    if (d < best) {
      best = d;
      *out = m;
      found = true;
    }
  }
  return found && best <= window_hop;
}

bool same_model(const SharedLpcModel &a, const SharedLpcModel &b) {
  if (a.timestamp != b.timestamp || a.order != b.order ||
      a.valid != b.valid)
    return false;
  if (std::memcmp(&a.confidence, &b.confidence, sizeof(float)) != 0)
    return false;
  if (std::memcmp(&a.prediction_error, &b.prediction_error, sizeof(float)) != 0)
    return false;
  for (size_t i = 0; i < a.coefficients.size(); ++i)
    if (std::memcmp(&a.coefficients[i], &b.coefficients[i], sizeof(float)) != 0)
      return false;
  return true;
}

} // namespace

int main() {
  SharedLpcAnalysis lpc;
  LpcConfig cfg{};
  cfg.window_size = 1024;
  cfg.hop_size = 384;
  cfg.order = 16;
  if (!lpc.init(48000.0f, cfg)) {
    std::printf("B4D5_MODEL_EQUIVALENCE init=FAIL\n");
    return 1;
  }

  PitchResult pitch{};
  pitch.voiced = true;
  pitch.confidence = 1.0f;

  std::vector<float> block(64);
  double phase = 0.0;
  for (int b = 0; b < 600; ++b) {
    for (size_t i = 0; i < block.size(); ++i) {
      block[i] = 0.3f * static_cast<float>(std::sin(phase));
      phase += 2.0 * 3.14159265358979323846 * 220.0 / 48000.0;
    }
    lpc.tap(block.data(), block.size());
    lpc.run(8, pitch);
  }

  std::vector<SharedLpcModel> snapshot(SharedLpcAnalysis::kModelCount);
  const size_t n = lpc.snapshot_models(snapshot.data(), snapshot.size());
  if (n < 2) {
    std::printf("B4D5_MODEL_EQUIVALENCE published=%zu result=FAIL (too few)\n",
                n);
    return 1;
  }
  snapshot.resize(n);

  const uint64_t window_hop = cfg.window_size + cfg.hop_size;
  uint64_t identity_mismatch = 0, coeff_mismatch = 0, cases = 0;
  uint64_t lo = snapshot[0].timestamp, hi = snapshot[0].timestamp;
  for (const auto &m : snapshot) {
    lo = m.timestamp < lo ? m.timestamp : lo;
    hi = m.timestamp > hi ? m.timestamp : hi;
  }
  const int64_t pad = static_cast<int64_t>(window_hop) * 3;

  for (int64_t ts = static_cast<int64_t>(lo) - pad;
       ts <= static_cast<int64_t>(hi) + pad; ++ts) {
    SharedLpcModel got{}, want{};
    const bool got_ok =
        lpc.model_near(static_cast<uint64_t>(ts < 0 ? 0 : ts), &got);
    const bool want_ok = ref_model_near(snapshot.data(), snapshot.size(),
                                        static_cast<uint64_t>(ts < 0 ? 0 : ts),
                                        &want, window_hop);
    ++cases;
    if (got_ok != want_ok) {
      ++identity_mismatch;
      continue;
    }
    if (!got_ok) continue;
    if (!same_model(got, want))
      ++coeff_mismatch;
  }

  std::printf("B4D5_MODEL_EQUIVALENCE cases=%llu published=%zu "
              "selected_model_mismatch=%llu coefficient_mismatch=%llu "
              "result=%s\n",
              static_cast<unsigned long long>(cases), n,
              static_cast<unsigned long long>(identity_mismatch),
              static_cast<unsigned long long>(coeff_mismatch),
              (identity_mismatch == 0 && coeff_mismatch == 0) ? "PASS"
                                                              : "FAIL");
  return (identity_mismatch == 0 && coeff_mismatch == 0) ? 0 : 1;
}
