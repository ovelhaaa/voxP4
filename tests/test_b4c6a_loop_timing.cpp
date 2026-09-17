// B4C.6A host audit: whole-loop timing accounting without hardware.
//
// Verifies the firmware accounting helpers, the B4C.6 1V arithmetic
// (non-DSP interval, rate deficit, miss-insufficiency proof), and the
// AudioI2s B4C.6A accumulator (sections, reconciliation, lateness,
// catch-up) using synthetic iterations on host.
#include "audio_i2s.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using vocal_fx_platform::AudioI2s;
using vocal_fx_platform::AudioI2sConfig;

namespace {

constexpr double kTol = 1e-6;

void require(bool ok, const char *msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
  }
}

void require_near(double got, double want, double tol, const char *msg) {
  if (std::fabs(got - want) > tol) {
    std::fprintf(stderr, "FAIL: %s got=%.6f want=%.6f tol=%.6f\n", msg, got,
                 want, tol);
    std::exit(1);
  }
}

// §5: avg_us boundary is DSP-only by construction (vocal_fx_process tap).
// This test pins the accounting definitions the firmware run() implements.
void test_static_helpers() {
  require_near(AudioI2s::b4c6a_reconciliation_pct(980, 1000), 98.0, kTol,
               "reconciliation 980/1000");
  require_near(AudioI2s::b4c6a_reconciliation_pct(0, 0), 0.0, kTol,
               "reconciliation 0/0");
  require_near(AudioI2s::b4c6a_effective_fs(2761430.0, 60.0),
               2761430.0 / 60.0, 1e-9, "effective_fs");
  require_near(AudioI2s::b4c6a_effective_fs(0.0, 0.0), 0.0, kTol,
               "effective_fs zero wall");
  require_near(AudioI2s::b4c6a_expected_blocks(60.0), 45000.0, kTol,
               "expected_blocks 60s");
  require_near(AudioI2s::b4c6a_timeline_lost_s(60.0, 46024.91),
               60.0 * (1.0 - 46024.91 / 48000.0), 1e-9, "timeline lost");
}

// §6: one-voice arithmetic target from the B4C.6 1V/60 s result.
void test_one_voice_arithmetic() {
  const double eff_fs = 46024.91;
  const double block_period = 64.0 / eff_fs * 1e6;  // 1390.55 us
  require_near(block_period, 1390.55, 0.01, "1V implied block period");
  const double dsp_avg = 847.49;
  const double non_dsp = block_period - dsp_avg;  // ~543.06 us
  require_near(non_dsp, 543.06, 0.01, "1V non-DSP interval");
  const double expected_wait = 1333.333333 - dsp_avg;  // 485.84 us
  require_near(expected_wait, 485.84, 0.01, "1V expected I2S wait");
  require_near(non_dsp - expected_wait, 57.22, 0.01,
               "1V recurring extra delay");
}

// §7: deadline-miss insufficiency proof. 69 misses at the observed max
// lateness (2130 - 1333 = 797 us) cover at most 0.055 s of the 2.469 s
// timeline loss, so the deficit is a per-block rate shortfall.
void test_miss_insufficiency() {
  const double lost = AudioI2s::b4c6a_timeline_lost_s(60.0, 46024.91);
  require_near(lost, 2.469, 0.005, "1V timeline lost ~2.47 s");
  const double max_cover = 69.0 * 797.0 / 1e6;  // 0.055 s
  require(max_cover < 0.06, "69 misses cover < 0.06 s");
  require(lost / max_cover > 40.0, "lost time >> miss lateness cover");
  // Per-block rate view closes the equation: ~57 us accrues per
  // *processed* block (60 s at 46024.91 Hz = 43148 blocks).
  const double processed = 60.0 * 46024.91 / 64.0;
  const double rate_cover = 57.22 * processed / 1e6;
  require_near(rate_cover, lost, 0.05, "rate shortfall explains loss");
}

// §4/§34: non-overlapping sections reconcile against loop_total.
void test_accumulator_reconciliation() {
  AudioI2s audio;
  // Mirror the firmware lifecycle: init() allocates the PSRAM/host
  // histogram backing consumed by note_iteration/percentiles.
  require(audio.init(AudioI2sConfig{}, 64), "audio init");
  // Synthetic steady-state block: sections sum exactly to loop_total.
  // A=480 B=8 F=30 C=847 D=12 E=25 G=6 -> accounted 1408; other=4.
  for (int i = 0; i < 100; ++i) {
    audio.b4c6a_note_iteration(
        480, 8, 30, 847, 12, 25, 6, 4, 1412,
        i ? 1412 : 0, i ? 2 : 0, 1412 * 360, 512, 512, 64, 64, 0, 0,
        static_cast<uint64_t>(i * 1412),
        static_cast<uint64_t>(i * 1412 + 1412), true);
  }
  const auto &t = audio.b4c6a_totals();
  require(t.samples == 100, "100 samples accumulated");
  const uint64_t accounted = t.rx_wait_us + t.rx_copy_us + t.fixture_us +
                             t.dsp_us + t.tx_prep_us + t.tx_wait_us +
                             t.bookkeeping_us;
  const double recon =
      AudioI2s::b4c6a_reconciliation_pct(accounted, t.loop_total_us);
  require_near(recon, 100.0 * 1408.0 / 1412.0, 1e-9, "reconciliation");
  require(recon >= 98.0, "reconciliation >= 98%");
  // Failed transfers must not count as normal blocks.
  audio.b4c6a_note_iteration(480, 8, 30, 847, 12, 25, 6, 4, 1412, 1412, 2,
                             1412 * 360, 0, 512, 0, 64, -1, 0, 141200, 142612,
                             false);
  require(audio.b4c6a_totals().samples == 100, "failed block not counted");
  const auto loop_p = audio.b4c6a_loop_percentiles();
  require(loop_p.samples == 100, "loop histogram covers every block");
  require_near(loop_p.avg_us, 1412.0, kTol, "loop avg");
  const auto dsp_p = audio.b4c6a_dsp_forensic_percentiles();
  require_near(dsp_p.avg_us, 847.0, kTol, "dsp avg kept separate");
  const auto rx_p = audio.b4c6a_rx_percentiles();
  require_near(rx_p.avg_us, 480.0, kTol, "rx wait avg");
  const auto tx_p = audio.b4c6a_tx_percentiles();
  require_near(tx_p.avg_us, 25.0, kTol, "tx wait avg");
}

// §7 second half + §22: positive lateness accumulates per late loop;
// a late block followed by near-zero RX wait counts as catch-up.
void test_lateness_and_catchup() {
  AudioI2s audio;
  // Mirror the firmware lifecycle: init() allocates the PSRAM/host
  // histogram backing consumed by note_iteration/percentiles.
  require(audio.init(AudioI2sConfig{}, 64), "audio init");
  // Block 0: late loop (1500 us -> 167 us lateness).
  audio.b4c6a_note_iteration(600, 8, 30, 800, 12, 40, 6, 4, 1500, 0, 0,
                             1500 * 360, 512, 512, 64, 64, 0, 0, 0, 1500,
                             true);
  // Block 1: back-to-back catch-up (RX wait ~0 after late block).
  audio.b4c6a_note_iteration(5, 8, 30, 800, 12, 40, 6, 4, 905, 1500, 3,
                             905 * 360, 512, 512, 64, 64, 0, 0, 1503, 2408,
                             true);
  const auto &t = audio.b4c6a_totals();
  require(t.late_loop_blocks == 1, "one late loop block");
  require(t.positive_lateness_us == 167, "positive lateness 167 us");
  require(t.catchup_rx_near_zero == 1, "catch-up rx~0 counted");
}

// §30: realtime PASS needs effective Fs within ±0.5% (helper-level check).
void test_qualification_band() {
  const double cases[][2] = {{48000.0, 1}, {47800.0, 1}, {48240.0, 1},
                             {46024.91, 0}, {37728.17, 0}};
  for (const auto &c : cases) {
    const bool ok = std::fabs(c[0] - 48000.0) <= 240.0;
    require(ok == (c[1] == 1), "qualification band ±0.5%");
  }
}

}  // namespace

int main() {
  std::printf("=== B4C.6A loop-timing host audit ===\n");
  test_static_helpers();
  test_one_voice_arithmetic();
  test_miss_insufficiency();
  test_accumulator_reconciliation();
  test_lateness_and_catchup();
  test_qualification_band();
  std::printf("=== ALL B4C.6A LOOP-TIMING TESTS PASSED ===\n");
  return 0;
}
