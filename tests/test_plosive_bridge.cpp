#include "harmony_plosive_bridge.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static void test_b0_none() {
  HarmonyPlosiveBridge bridge;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B0_None;
  bridge.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.0f;

  for (int i = 0; i < 480; ++i) {
    float src = (i % 2 == 0) ? 0.5f : -0.5f;
    float out_comp = 1.0f;
    float out = bridge.process_sample(src, pitch, PitchTrackState::Unlocked, 0.2f, &out_comp);
    assert(out == 0.0f);
    assert(out_comp == 0.0f);
    assert(!bridge.is_active());
  }
  std::printf("PASS: test_b0_none\n");
}

static void test_phrase_start_detection() {
  HarmonyPlosiveBridge bridge;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B4_BroadbandTransient;
  cfg.phrase_start_silence_ms = 80.0f;
  cfg.phrase_start_gain_multiplier = 1.5f;
  bridge.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.0f;

  // Feed 100 ms of pure silence (4800 samples at 48 kHz)
  for (int i = 0; i < 4800; ++i) {
    bridge.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
    assert(!bridge.is_active());
  }

  // Now feed a sharp plosive transient: high step / high derivative with energy
  bool triggered = false;
  for (int i = 0; i < 240; ++i) {
    float src = (i == 0) ? 0.6f : ((i % 4 < 2) ? 0.4f : -0.4f);
    float out = bridge.process_sample(src, pitch, PitchTrackState::Unlocked, 0.3f);
    (void)out;
    if (bridge.is_active()) {
      triggered = true;
      assert(bridge.is_phrase_start());
      assert(bridge.current_gain() > 0.0f);
      break;
    }
  }
  assert(triggered);
  std::printf("PASS: test_phrase_start_detection\n");
}

static void test_mid_phrase_plosive() {
  HarmonyPlosiveBridge bridge;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B4_BroadbandTransient;
  cfg.phrase_start_silence_ms = 80.0f;
  cfg.phrase_start_gain_multiplier = 1.5f;
  cfg.transient_gain = 0.20f;
  bridge.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.05f;

  // Active vocal signal for 500 samples, then brief 15 ms gap (720 samples < 80ms)
  for (int i = 0; i < 500; ++i) {
    bridge.process_sample(0.1f, pitch, PitchTrackState::Unlocked, 0.1f);
  }
  for (int i = 0; i < 720; ++i) {
    bridge.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
  }

  // Now trigger plosive
  bool triggered = false;
  for (int i = 0; i < 240; ++i) {
    float src = (i == 0) ? 0.6f : ((i % 4 < 2) ? 0.4f : -0.4f);
    bridge.process_sample(src, pitch, PitchTrackState::Unlocked, 0.3f);
    if (bridge.is_active()) {
      triggered = true;
      // Should NOT be phrase-start because gap was only 15 ms (< 80 ms)
      assert(!bridge.is_phrase_start());
      break;
    }
  }
  assert(triggered);
  std::printf("PASS: test_mid_phrase_plosive\n");
}

static void test_silence_and_vocal_fry_rejection() {
  HarmonyPlosiveBridge bridge;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B4_BroadbandTransient;
  bridge.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.0f;

  // 1. Pure silence
  for (int i = 0; i < 2000; ++i) {
    float out = bridge.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
    assert(out == 0.0f);
    assert(!bridge.is_active());
  }

  // 2. Slow 80 Hz vocal fry sine wave (smooth, low derivative)
  for (int i = 0; i < 2000; ++i) {
    float s = std::sin(2.0f * 3.14159f * 80.0f * i / 48000.0f) * 0.3f;
    float out = bridge.process_sample(s, pitch, PitchTrackState::Unlocked, 0.2f);
    assert(out == 0.0f);
    assert(!bridge.is_active());
  }
  std::printf("PASS: test_silence_and_vocal_fry_rejection\n");
}

static void test_sustained_voiced_rejection() {
  HarmonyPlosiveBridge bridge;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B4_BroadbandTransient;
  bridge.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = true;
  pitch.confidence = 0.95f;
  pitch.frequency_hz = 220.0f;

  // Even with sharp transients, if pitch tracker is locked and confident voiced,
  // plosive bridge must not engage
  for (int i = 0; i < 2000; ++i) {
    float src = ((i % 4) < 2) ? 0.5f : -0.5f;
    float out = bridge.process_sample(src, pitch, PitchTrackState::Locked, 0.3f);
    assert(out == 0.0f);
    assert(!bridge.is_active());
  }
  std::printf("PASS: test_sustained_voiced_rejection\n");
}

static void test_bridge_duration_and_timeout() {
  HarmonyPlosiveBridge bridge;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B4_BroadbandTransient;
  cfg.transient_duration_ms = 3.0f; // 144 samples
  cfg.max_bridge_duration_ms = 15.0f; // 720 samples
  bridge.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.0f;

  // 100 ms silence to prime phrase-start
  for (int i = 0; i < 4800; ++i) {
    bridge.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
  }

  // Trigger burst
  int active_samples = 0;
  for (int i = 0; i < 2000; ++i) {
    float src = ((i % 4) < 2) ? 0.4f : -0.4f;
    float out = bridge.process_sample(src, pitch, PitchTrackState::Unlocked, 0.25f);
    if (bridge.is_active()) {
      active_samples++;
      assert(std::isfinite(out));
    }
  }

  // Active samples must be non-zero and must strictly not exceed max_bridge_duration_ms + grace
  assert(active_samples > 0);
  assert(active_samples <= 720);
  assert(!bridge.is_active()); // Must have timed out and turned off

  std::printf("PASS: test_bridge_duration_and_timeout (active=%d samples / %.1f ms)\n",
              active_samples, active_samples * 1000.0f / 48000.0f);
}

static void test_prng_determinism_and_independence() {
  HarmonyPlosiveBridge b0, b1;
  HarmonyPlosiveBridgeConfig cfg;
  cfg.policy = PlosiveBridgePolicy::B4_BroadbandTransient;
  cfg.transient_duration_ms = 1.0f; // quickly enter Phase 2 noise
  cfg.max_bridge_duration_ms = 15.0f;

  b0.init(48000.0f, 0, cfg);
  b1.init(48000.0f, 1, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.0f;

  for (int i = 0; i < 4800; ++i) {
    b0.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
    b1.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
  }

  std::vector<float> out0(1000), out1(1000);
  for (int i = 0; i < 1000; ++i) {
    float src = ((i % 4) < 2) ? 0.4f : -0.4f;
    out0[i] = b0.process_sample(src, pitch, PitchTrackState::Unlocked, 0.25f);
    out1[i] = b1.process_sample(src, pitch, PitchTrackState::Unlocked, 0.25f);
  }

  // Phase 2 noise should be different between voices
  bool different = false;
  for (int i = 100; i < 500; ++i) {
    if (std::fabs(out0[i] - out1[i]) > 1e-4f) {
      different = true;
      break;
    }
  }
  assert(different);

  // Determinism check: reset b0 and re-run
  b0.reset();
  for (int i = 0; i < 4800; ++i) {
    b0.process_sample(0.0f, pitch, PitchTrackState::Unlocked, 0.0f);
  }
  for (int i = 0; i < 1000; ++i) {
    float src = ((i % 4) < 2) ? 0.4f : -0.4f;
    float repeat = b0.process_sample(src, pitch, PitchTrackState::Unlocked, 0.25f);
    assert(std::fabs(repeat - out0[i]) < 1e-6f);
  }

  std::printf("PASS: test_prng_determinism_and_independence\n");
}

int main() {
  test_b0_none();
  test_phrase_start_detection();
  test_mid_phrase_plosive();
  test_silence_and_vocal_fry_rejection();
  test_sustained_voiced_rejection();
  test_bridge_duration_and_timeout();
  test_prng_determinism_and_independence();
  std::printf("All Plosive Bridge unit tests passed successfully.\n");
  return 0;
}
