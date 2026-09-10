#include "harmony_unvoiced_articulation.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static void test_u0_silence() {
  HarmonyUnvoicedArticulation art;
  HarmonyUnvoicedArticulationConfig cfg;
  cfg.mode = UnvoicedArticulationMode::U0_Silence;
  art.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.1f;

  for (int i = 0; i < 480; ++i) {
    float src = (i % 2 == 0) ? 0.5f : -0.5f;
    float out_comp = 1.0f;
    float out = art.process_sample(src, pitch, PitchTrackState::Unlocked, 0.1f, &out_comp);
    assert(out == 0.0f);
    assert(out_comp == 0.0f);
  }
  std::printf("PASS: test_u0_silence\n");
}

static void test_prng_determinism_and_independence() {
  HarmonyUnvoicedArticulation art0;
  HarmonyUnvoicedArticulation art1;
  HarmonyUnvoicedArticulationConfig cfg;
  cfg.mode = UnvoicedArticulationMode::U4_NoiseExcitation;
  cfg.transition_ms = 0.5f;

  art0.init(48000.0f, 0, cfg);
  art1.init(48000.0f, 1, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.05f;

  std::vector<float> v0(1000), v1(1000);
  for (size_t i = 0; i < 1000; ++i) {
    // High-frequency alternating input to trigger non-periodic classification
    float src = ((i % 4) < 2) ? 0.3f : -0.3f;
    v0[i] = art0.process_sample(src, pitch, PitchTrackState::Unlocked, 0.2f);
    v1[i] = art1.process_sample(src, pitch, PitchTrackState::Unlocked, 0.2f);
  }

  // Check independence (voices must not produce identical noise)
  bool different = false;
  for (size_t i = 100; i < 1000; ++i) {
    if (std::fabs(v0[i] - v1[i]) > 1e-4f) {
      different = true;
      break;
    }
  }
  assert(different);

  // Check determinism: reset art0 and verify it reproduces bit-exact v0
  art0.reset();
  for (size_t i = 0; i < 1000; ++i) {
    float src = ((i % 4) < 2) ? 0.3f : -0.3f;
    float repeat = art0.process_sample(src, pitch, PitchTrackState::Unlocked, 0.2f);
    assert(std::fabs(repeat - v0[i]) < 1e-6f);
  }
  std::printf("PASS: test_prng_determinism_and_independence\n");
}

static void test_acoustic_classification() {
  HarmonyUnvoicedArticulation art;
  HarmonyUnvoicedArticulationConfig cfg;
  cfg.mode = UnvoicedArticulationMode::U4_NoiseExcitation;
  art.init(48000.0f, 0, cfg);

  // 1. Silence
  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.0f;
  for (int i = 0; i < 500; ++i) {
    art.process_sample(0.0001f, pitch, PitchTrackState::Unlocked, 0.0001f);
  }
  assert(art.acoustic_class() == UnvoicedAcousticClass::Silence);

  // 2. Confident periodic locked
  pitch.voiced = true;
  pitch.confidence = 0.95f;
  pitch.frequency_hz = 220.0f;
  for (int i = 0; i < 500; ++i) {
    float s = std::sin(2.0f * 3.14159f * 220.0f * i / 48000.0f) * 0.2f;
    art.process_sample(s, pitch, PitchTrackState::Locked, 0.15f);
  }
  assert(art.acoustic_class() == UnvoicedAcousticClass::Periodic);
  assert(!art.is_eligible());

  // 3. Low frequency tone / vocal fry (100 Hz tone, low ZCR)
  pitch.voiced = false;
  pitch.confidence = 0.15f;
  for (int i = 0; i < 2000; ++i) {
    float s = std::sin(2.0f * 3.14159f * 100.0f * i / 48000.0f) * 0.2f;
    art.process_sample(s, pitch, PitchTrackState::Unlocked, 0.15f);
  }
  // Low ZCR and low HPF energy should classify as VocalFryLowFreq, NOT NonPeriodic
  assert(art.acoustic_class() == UnvoicedAcousticClass::VocalFryLowFreq);
  assert(!art.is_eligible());

  // 4. Non-periodic high frequency burst (fricative-like: 5 kHz tone or alternating)
  for (int i = 0; i < 2000; ++i) {
    float s = std::sin(2.0f * 3.14159f * 5000.0f * i / 48000.0f) * 0.2f;
    art.process_sample(s, pitch, PitchTrackState::Unlocked, 0.15f);
  }
  assert(art.acoustic_class() == UnvoicedAcousticClass::GenuinelyNonPeriodic);
  assert(art.is_eligible());

  std::printf("PASS: test_acoustic_classification\n");
}

static void test_synthetic_stimuli_stability() {
  HarmonyUnvoicedArticulation art;
  HarmonyUnvoicedArticulationConfig cfg;
  cfg.mode = UnvoicedArticulationMode::U4_NoiseExcitation;
  cfg.transition_ms = 5.0f;
  art.init(48000.0f, 0, cfg);

  PitchResult pitch{};
  pitch.voiced = false;
  pitch.confidence = 0.1f;

  // Run 10000 samples of alternating silence, noise, sine
  for (int i = 0; i < 10000; ++i) {
    float in = 0.0f;
    if (i >= 1000 && i < 3000) {
      in = ((i % 4) < 2 ? 0.4f : -0.4f); // noise burst
    } else if (i >= 5000 && i < 8000) {
      in = std::sin(2.0f * 3.14159f * 400.0f * i / 48000.0f) * 0.3f; // sine
    }
    float out = art.process_sample(in, pitch, PitchTrackState::Unlocked, std::fabs(in));
    assert(std::isfinite(out));
    assert(out >= -1.5f && out <= 1.5f);
  }
  std::printf("PASS: test_synthetic_stimuli_stability\n");
}

int main() {
  test_u0_silence();
  test_prng_determinism_and_independence();
  test_acoustic_classification();
  test_synthetic_stimuli_stability();
  std::printf("All Unvoiced Articulation unit tests passed successfully.\n");
  return 0;
}
