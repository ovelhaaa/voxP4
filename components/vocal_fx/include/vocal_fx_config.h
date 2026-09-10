#pragma once
#include "vocal_fx_types.h"
#include "lpc.h"
#include "harmony_articulation_envelope.h"
#include <cstdint>

constexpr float VOCAL_FX_DEFAULT_SAMPLE_RATE = 48000.0f;
constexpr uint32_t VOCAL_FX_DEFAULT_BLOCK_SIZE = 64;
constexpr uint32_t VOCAL_FX_MAX_BLOCK_SIZE = 256;
constexpr float VOCAL_FX_MAX_DELAY_SECONDS = 2.0f;
constexpr float VOCAL_FX_MAX_SAMPLE_RATE = 48000.0f;

struct VocalFxConfig {
  float sample_rate = VOCAL_FX_DEFAULT_SAMPLE_RATE;
  uint32_t block_size = VOCAL_FX_DEFAULT_BLOCK_SIZE;
  bool enable_gate = true;
  bool enable_compressor = true;
  bool enable_delay = true;
  bool enable_reverb = true;
  bool enable_pitch_analysis = true;
  // Diagnostic host path; false preserves the product mixer and input HPF.
  bool isolate_pitch_shift_output = false;
  // Host-only diagnostics. Defaults preserve the existing voice-1 renderer.
  uint8_t isolated_pitch_shift_voice = 0;
  bool apply_isolated_voice_envelope = false;
  IsolatedPitchShiftStem isolated_pitch_shift_stem =
      IsolatedPitchShiftStem::Final;
  // Experimental A/B switch; true is the product behaviour.
  bool voice2_onset_unvoiced_attenuation = true;
  // Host diagnostic; zero disables the short pitch-loss grace experiment.
  uint32_t voice2_short_pitch_loss_grace_samples = 0;
  // Host-selectable continuity experiment. Baseline preserves production.
  PsolaContinuityPolicy psola_continuity_policy =
      PsolaContinuityPolicy::Baseline;
  PsolaRecoveryMode psola_recovery_mode = PsolaRecoveryMode::Soft;
  float psola_coast_ms = 0.0f;
  float psola_recovery_crossfade_ms = 5.0f;
  float psola_recovery_small_error_cents = 50.0f;
  float psola_recovery_note_change_cents = 150.0f;
  bool warm_reacquire_enabled = false;
  WarmReacquireMode warm_reacquire_mode = WarmReacquireMode::W0_Current;
  float warm_reacquire_window_ms = 120.0f;
  float warm_confidence_margin = 0.0f;
  bool warm_mark_seed_enabled = false;
  HarmonyArticulationConfig voice2_articulation{};
  PitchShiftConfig pitch_shift{};
  LpcConfig lpc{};
};

enum class PitchDetectorAlgorithm : uint8_t { Yin, Mpm };

struct PitchAnalysisConfig {
  float input_sample_rate = 48000.0f;
  float analysis_sample_rate = 12000.0f;
  float min_frequency = 65.0f;
  float max_frequency = 1000.0f;
  uint32_t window_size = 512;
  uint32_t hop_size = 60;
  float yin_threshold = 0.15f;
  float voiced_enter_confidence = 0.80f;
  float voiced_exit_confidence = 0.60f;
  float min_input_db = -55.0f;
  float smoothing_ms = 30.0f;
  uint8_t voiced_attack_frames = 2;
  uint8_t voiced_release_frames = 3;
  float pitch_change_cents = 75.0f;
  float onset_ratio = 2.5f;
  PitchDetectorAlgorithm algorithm = PitchDetectorAlgorithm::Yin;
  PsolaContinuityPolicy continuity_policy = PsolaContinuityPolicy::Baseline;
  float coast_ms = 0.0f;
  bool warm_reacquire_enabled = false;
  WarmReacquireMode warm_reacquire_mode = WarmReacquireMode::W0_Current;
  float warm_reacquire_window_ms = 120.0f;
  float warm_confidence_margin = 0.0f;
  bool warm_mark_seed_enabled = false;
};
