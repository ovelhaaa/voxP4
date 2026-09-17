#include "td_psola.h"
#include "lpc.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

namespace {
constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

size_t marks_for(uint64_t available, float period,
                 std::array<PitchMark, 64> &marks) {
  const uint64_t last_index =
      static_cast<uint64_t>(std::floor(available / period));
  const uint64_t first_index = last_index > 62 ? last_index - 62 : 0;
  size_t n = 0;
  for (uint64_t index = first_index; index <= last_index && n < marks.size();
       ++index) {
    marks[n++] = {static_cast<uint64_t>(std::llround(index * period)), 1, false};
  }
  return n;
}

struct TestResult {
  double max_diff = 0.0;
  double snr_db = 0.0;
  bool bit_identical = true;
};

TestResult compare_runs(const std::vector<float> &ref, const std::vector<float> &cand) {
  require(ref.size() == cand.size(), "size match");
  double max_diff = 0.0;
  double err_energy = 0.0;
  double sig_energy = 0.0;
  bool bit_identical = true;

  for (size_t i = 0; i < ref.size(); ++i) {
    const float r = ref[i];
    const float c = cand[i];
    if (r != c) {
      bit_identical = false;
    }
    const double diff = std::fabs(static_cast<double>(c) - static_cast<double>(r));
    if (diff > max_diff) {
      max_diff = diff;
    }
    err_energy += diff * diff;
    sig_energy += static_cast<double>(r) * static_cast<double>(r);
  }

  double snr_db = 999.0;
  if (err_energy > 1e-20) {
    snr_db = 10.0 * std::log10((sig_energy + 1e-20) / err_energy);
  }
  return {max_diff, snr_db, bit_identical};
}

void run_case(const std::string &name, const std::vector<float> &input,
              float f0, float semitones, FormantMode formant_mode) {
  constexpr size_t block = 64;
  const size_t total = input.size();
  const float period = f0 > 0.0f ? kRate / f0 : 100.0f;

  // Set up shared LPC analysis
  SharedLpcAnalysis lpc_analysis;
  LpcConfig lpc_cfg;
  lpc_cfg.order = 10;
  require(lpc_analysis.init(kRate, lpc_cfg), "lpc init");

  // Populate LPC analysis with input signal
  PitchResult pr_dummy{};
  pr_dummy.voiced = (f0 > 0.0f);
  pr_dummy.confidence = 1.0f;
  pr_dummy.frequency_hz = f0;
  pr_dummy.period_samples = period;
  for (size_t pos = 0; pos < total; pos += block) {
    lpc_analysis.tap(input.data() + pos, std::min(block, total - pos));
    (void)lpc_analysis.run(4, pr_dummy);
  }

  auto render = [&](PsolaLpcKernel lpc_k, PsolaOlaKernel ola_k,
                    PsolaGrainKernel grain_k, PsolaSynthesisKernel synth_k) -> std::vector<float> {
    SharedPitchShiftResources shared;
    shared.init();
    TdPsola p;
    PitchShiftConfig c{};
    c.enabled = true;
    c.semitones = semitones;
    c.wet = 1.0f;
    c.smoothing_ms = 1.0f; // fast convergence for test
    c.formant_mode = formant_mode;
    c.formant_amount = 1.0f;
    c.formant_shift_semitones = 3.0f;
    c.formant_bandwidth_expansion = 0.985f;
    c.lpc_kernel = lpc_k;
    c.ola_kernel = ola_k;
    c.grain_kernel = grain_k;
    c.synthesis_kernel = synth_k;

    require(p.init(kRate, c, &shared, &lpc_analysis), "td_psola init");

    std::vector<float> out(total, 0.0f);
    for (size_t pos = 0; pos < total; pos += block) {
      const size_t n = std::min(block, total - pos);
      std::array<PitchMark, 64> m{};
      const uint64_t analyzed = pos > 1100 ? pos - 1100 : 0;
      size_t count = 0;
      PitchResult r{};
      if (f0 > 0.0f) {
        count = marks_for(analyzed, period, m);
        r.voiced = true;
        r.confidence = 1.0f;
        r.frequency_hz = f0;
        r.period_samples = period;
        r.analysis_timestamp_samples = analyzed;
      }
      p.process(input.data() + pos, out.data() + pos, n, r,
                count >= 3 ? PitchTrackState::Locked : PitchTrackState::Acquiring,
                m.data(), count);
    }
    return out;
  };

  // 1. Golden Reference
  const auto ref = render(PsolaLpcKernel::Reference, PsolaOlaKernel::Reference,
                          PsolaGrainKernel::Reference, PsolaSynthesisKernel::Reference);

  // 2. Candidate A: ContiguousExact
  const auto cand_a = render(PsolaLpcKernel::ContiguousExact, PsolaOlaKernel::Reference,
                             PsolaGrainKernel::Reference, PsolaSynthesisKernel::Reference);
  auto res_a = compare_runs(ref, cand_a);
  std::printf("  [%s] Cand A (ContiguousExact) vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_a.bit_identical, res_a.max_diff, res_a.snr_db);
  require(res_a.bit_identical, "Candidate A must be bit-identical to reference");

  // 3. Candidate B: ContiguousMulti4
  const auto cand_b = render(PsolaLpcKernel::ContiguousMulti4, PsolaOlaKernel::Reference,
                             PsolaGrainKernel::Reference, PsolaSynthesisKernel::Reference);
  auto res_b = compare_runs(ref, cand_b);
  std::printf("  [%s] Cand B (ContiguousMulti4) vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_b.bit_identical, res_b.max_diff, res_b.snr_db);
  require(res_b.bit_identical, "Candidate B must be bit-identical to reference");

  // 4. Candidate C: ContiguousMulti8
  const auto cand_c = render(PsolaLpcKernel::ContiguousMulti8, PsolaOlaKernel::Reference,
                             PsolaGrainKernel::Reference, PsolaSynthesisKernel::Reference);
  auto res_c = compare_runs(ref, cand_c);
  std::printf("  [%s] Cand C (ContiguousMulti8) vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_c.bit_identical, res_c.max_diff, res_c.snr_db);
  require(res_c.bit_identical, "Candidate C must be bit-identical to reference");

  // 5. Candidate D: OlaContiguous
  const auto cand_d = render(PsolaLpcKernel::ContiguousExact, PsolaOlaKernel::Contiguous,
                             PsolaGrainKernel::Reference, PsolaSynthesisKernel::Reference);
  auto res_d = compare_runs(ref, cand_d);
  std::printf("  [%s] Cand D (OlaContiguous) vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_d.bit_identical, res_d.max_diff, res_d.snr_db);
  require(res_d.bit_identical, "Candidate D must be bit-identical to reference");

  // 6. Candidate E4: CombinedMulti4
  const auto cand_e4 = render(PsolaLpcKernel::ContiguousExact, PsolaOlaKernel::Contiguous,
                              PsolaGrainKernel::ContiguousMulti4, PsolaSynthesisKernel::Reference);
  auto res_e4 = compare_runs(ref, cand_e4);
  std::printf("  [%s] Cand E4 (CombinedMulti4) vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_e4.bit_identical, res_e4.max_diff, res_e4.snr_db);
  require(res_e4.bit_identical, "Candidate E4 must be bit-identical to reference");

  // 7. Candidate E8: CombinedMulti8
  const auto cand_e8 = render(PsolaLpcKernel::ContiguousExact, PsolaOlaKernel::Contiguous,
                              PsolaGrainKernel::ContiguousMulti8, PsolaSynthesisKernel::Reference);
  auto res_e8 = compare_runs(ref, cand_e8);
  std::printf("  [%s] Cand E8 (CombinedMulti8) vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_e8.bit_identical, res_e8.max_diff, res_e8.snr_db);
  require(res_e8.bit_identical, "Candidate E8 must be bit-identical to reference");

  // 8. Synthesis UnrolledExact
  const auto cand_syn = render(PsolaLpcKernel::Reference, PsolaOlaKernel::Reference,
                               PsolaGrainKernel::Reference, PsolaSynthesisKernel::UnrolledExact);
  auto res_syn = compare_runs(ref, cand_syn);
  std::printf("  [%s] Synth Unrolled vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_syn.bit_identical, res_syn.max_diff, res_syn.snr_db);
  require(res_syn.bit_identical, "Synthesis UnrolledExact must be bit-identical to reference");

  // 9. All Combined Winners
  const auto cand_all = render(PsolaLpcKernel::ContiguousMulti8, PsolaOlaKernel::Contiguous,
                               PsolaGrainKernel::ContiguousMulti8, PsolaSynthesisKernel::UnrolledExact);
  auto res_all = compare_runs(ref, cand_all);
  std::printf("  [%s] Full Optimized vs Ref: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              name.c_str(), res_all.bit_identical, res_all.max_diff, res_all.snr_db);
  require(res_all.bit_identical, "Full Optimized configuration must be bit-identical to reference");
}

} // namespace

int main() {
  std::printf("=== TD-PSOLA B4C.2 Kernel Bit-Exact Qualification ===\n");

  constexpr size_t total = 48000 * 2; // 2 seconds

  // Test 1: Silence
  {
    std::printf("\n--- Test: Silence (zeros) ---\n");
    std::vector<float> silence(total, 0.0f);
    run_case("Silence FormantOff", silence, 220.0f, 4.0f, FormantMode::Off);
    run_case("Silence FormantLpc", silence, 220.0f, 4.0f, FormantMode::Lpc);
  }

  // Test 2: Harmonic tones at 65 Hz, 110 Hz, 220 Hz, 440 Hz
  for (float f0 : {65.0f, 110.0f, 220.0f, 440.0f}) {
    std::printf("\n--- Test: Pitched Tone %.1f Hz ---\n", f0);
    std::vector<float> sig(total, 0.0f);
    for (size_t i = 0; i < total; ++i) {
      for (int h = 1; h <= 5; ++h) {
        sig[i] += (0.15f / h) * std::sin(2.0f * kPi * f0 * h * i / kRate);
      }
    }
    for (float st : {-12.0f, -7.0f, 0.0f, +7.0f, +12.0f}) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "f0=%.0f st=%+.0f FormantOff", f0, st);
      run_case(buf, sig, f0, st, FormantMode::Off);

      std::snprintf(buf, sizeof(buf), "f0=%.0f st=%+.0f FormantLpc", f0, st);
      run_case(buf, sig, f0, st, FormantMode::Lpc);
    }
  }

  // Test 3: Pseudo-random noise
  {
    std::printf("\n--- Test: Pseudo-random Noise ---\n");
    std::vector<float> noise(total, 0.0f);
    uint32_t seed = 0x12345678;
    for (size_t i = 0; i < total; ++i) {
      seed = seed * 1664525u + 1013904223u;
      noise[i] = (static_cast<float>(seed) / static_cast<float>(0xFFFFFFFFu) - 0.5f) * 0.2f;
    }
    run_case("Noise FormantOff", noise, 220.0f, 3.0f, FormantMode::Off);
    run_case("Noise FormantLpc", noise, 220.0f, 3.0f, FormantMode::Lpc);
  }

  std::printf("\n=== ALL PSOLA KERNEL TESTS PASSED (100%% BIT-IDENTICAL) ===\n");
  return 0;
}
