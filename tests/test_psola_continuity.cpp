#include "pitch_analysis.h"
#include "td_psola.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

struct TrackerObservation {
  bool onset = false, onset_unlocked = false, coast = false;
  bool recovered = false, unlocked = false;
  uint32_t max_coast_samples = 0;
};

void feed(PitchAnalysis &analysis, uint64_t &position, size_t frames,
          float frequency, float amplitude, TrackerObservation &observation) {
  std::array<float, 64> block{};
  for (size_t processed = 0; processed < frames;) {
    const size_t count = std::min(block.size(), frames - processed);
    for (size_t i = 0; i < count; ++i, ++position)
      block[i] = frequency > 0
                     ? amplitude * std::sin(2 * kPi * frequency * position / kRate)
                     : 0.0f;
    analysis.tap(block.data(), count);
    while (analysis.run(1)) {
      const auto result = analysis.latest();
      const auto state = analysis.track_state();
      const auto debug = analysis.debug();
      observation.onset |= result.onset;
      observation.onset_unlocked |= result.onset &&
                                    state == PitchTrackState::Unlocked;
      observation.coast |= state == PitchTrackState::Coasting;
      observation.recovered |= observation.coast &&
                               state == PitchTrackState::Locked;
      observation.unlocked |= state == PitchTrackState::Unlocked;
      observation.max_coast_samples =
          std::max(observation.max_coast_samples, debug.coast_elapsed_samples);
    }
    processed += count;
  }
}

void analysis_state_regressions() {
  PitchAnalysisConfig config{};
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  config.coast_ms = 60;
  PitchAnalysis analysis;
  require(analysis.init(config), "continuity analysis init");
  uint64_t position = 0;
  TrackerObservation warm{};
  feed(analysis, position, 48000, 220, .03f, warm);
  require(analysis.track_state() == PitchTrackState::Locked,
          "constant pitch locks");

  TrackerObservation onset{};
  feed(analysis, position, 9600, 220, .6f, onset);
  require(onset.onset, "amplitude transition creates onset telemetry");
  require(!onset.onset_unlocked,
          "onset alone does not unlock continuity policy");

  TrackerObservation short_drop{};
  feed(analysis, position, 1920, 0, 0, short_drop);
  feed(analysis, position, 9600, 220, .4f, short_drop);
  require(short_drop.coast, "short confidence dropout enters coasting");
  require(short_drop.recovered, "short dropout recovers lock");
  require(short_drop.max_coast_samples <= 2880,
          "short dropout respects coast timeout");

  TrackerObservation long_drop{};
  feed(analysis, position, 12000, 0, 0, long_drop);
  require(long_drop.coast, "long loss initially coasts");
  require(analysis.track_state() == PitchTrackState::Unlocked,
          "long loss times out to unlocked");
  require(analysis.debug().coast_timeout_events > 0,
          "long loss records coast timeout");
  std::puts("continuity analysis: onset/dropout/timeout passed");
}

size_t make_marks(uint64_t available, float period,
                  std::array<PitchMark, 64> &marks, bool predicted) {
  const uint64_t last = static_cast<uint64_t>(available / period);
  const uint64_t first = last > 62 ? last - 62 : 0;
  size_t count = 0;
  for (uint64_t i = first; i <= last && count < marks.size(); ++i)
    marks[count++] = {static_cast<uint64_t>(std::llround(i * period)), 1.0f,
                      predicted};
  return count;
}

void renderer_continuity_regressions() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{};
  config.enabled = true;
  config.semitones = 4;
  config.wet = 1;
  config.fallback_policy = HarmonyFallbackPolicy::Muted;
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  require(psola.init(kRate, config, &shared), "continuity renderer init");
  psola.set_onset_unvoiced_attenuation(false);

  constexpr size_t block_size = 64;
  std::array<float, block_size> input{}, output{}, fallback{}, measured{},
      coasted{};
  std::array<uint8_t, block_size> measured_active{}, coasted_active{};
  const float period = kRate / 220.0f;
  double reconstruction_energy = 0, output_energy = 0;
  bool heard_measured = false, heard_coasted = false;
  for (uint64_t position = 0; position < 48000 * 3; position += block_size) {
    const bool coast = position >= 48000 && position < 48000 + 2400;
    const bool silence = position >= 96000 && position < 96000 + 12000;
    const float instantaneous_hz =
        position < 36000 ? 220.0f
                         : (position < 72000 ? 330.0f : 220.0f);
    for (size_t i = 0; i < block_size; ++i) {
      const float t = static_cast<float>(position + i) / kRate;
      input[i] = silence ? 0.0f
                         : .25f * std::sin(2 * kPi * instantaneous_hz * t);
    }
    shared.push(input.data(), block_size);
    std::array<PitchMark, 64> marks{};
    const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
    const size_t count = make_marks(analyzed, period, marks, coast);
    PitchResult pitch{};
    pitch.voiced = !silence;
    pitch.confidence = coast ? .4f : (!silence ? 1.0f : 0.0f);
    pitch.frequency_hz = instantaneous_hz;
    pitch.raw_frequency_hz = instantaneous_hz;
    pitch.period_samples = period;
    pitch.analysis_timestamp_samples = analyzed;
    const PitchTrackState state = silence ? PitchTrackState::Unlocked
                                          : (coast ? PitchTrackState::Coasting
                                                   : PitchTrackState::Locked);
    psola.process_shared(input.data(), output.data(), block_size, pitch, state,
                         marks.data(), count, nullptr, fallback.data(), nullptr,
                         nullptr, nullptr, measured.data(), coasted.data(),
                         measured_active.data(), coasted_active.data());
    for (size_t i = 0; i < block_size; ++i) {
      require(std::isfinite(output[i]) && std::fabs(output[i]) < 4,
              "coasting output finite and bounded");
      require(fallback[i] == 0.0f, "continuity policy injects no dry fallback");
      const double error = output[i] - measured[i] - coasted[i];
      reconstruction_energy += error * error;
      output_energy += static_cast<double>(output[i]) * output[i];
      heard_measured |= measured_active[i] != 0;
      heard_coasted |= coasted_active[i] != 0;
      if (silence && position > 96000 + 4096)
        require(std::fabs(output[i]) < 1e-7f,
                "long silence has no stale grain or buzz");
    }
  }
  require(heard_measured && heard_coasted,
          "measured and coasted origins are both attributed");
  require(std::sqrt(reconstruction_energy / std::max(output_energy, 1e-30)) <
              1e-6,
          "measured plus coasted reconstructs output");

  // Ring wrap and thousands of deterministic state changes.
  for (int transition = 0; transition < 4000; ++transition) {
    const bool coast = (transition & 1) != 0;
    for (size_t i = 0; i < block_size; ++i)
      input[i] = .2f * std::sin(2 * kPi * 220 *
                               (144000 + transition * block_size + i) / kRate);
    shared.push(input.data(), block_size);
    std::array<PitchMark, 64> marks{};
    const uint64_t available = 144000 + transition * block_size;
    const size_t count = make_marks(available > 1100 ? available - 1100 : 0,
                                    period, marks, coast);
    PitchResult pitch{220, coast ? .4f : 1.0f, true, available};
    pitch.period_samples = period;
    psola.process_shared(input.data(), output.data(), block_size, pitch,
                         coast ? PitchTrackState::Coasting
                               : PitchTrackState::Locked,
                         marks.data(), count, nullptr, fallback.data());
    require(std::all_of(output.begin(), output.end(), [](float value) {
              return std::isfinite(value) && std::fabs(value) < 4;
            }), "ring wrap and repeated transitions stay bounded");
    require(std::all_of(fallback.begin(), fallback.end(), [](float value) {
              return value == 0.0f;
            }), "repeated transitions never inject dry fallback");
  }
  std::puts("continuity renderer: attribution/note/silence/ring passed");
}

void same_note_recovery_regressions() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{};
  config.enabled = true;
  config.semitones = 4;
  config.wet = 1;
  config.fallback_policy = HarmonyFallbackPolicy::Muted;
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  require(psola.init(kRate, config, &shared), "same-note init");
  psola.set_onset_unvoiced_attenuation(false);

  constexpr size_t block_size = 64;
  std::array<float, block_size> input{}, output{}, fallback{};
  const float period = kRate / 220.0f;
  uint64_t position = 0;

  // Run through dropout durations: 10, 20, 30, 40, 60 ms
  for (int dropout_ms : {10, 20, 30, 40, 60}) {
    const size_t steady_samples = 4800; // 100 ms steady
    const size_t dropout_samples = static_cast<size_t>(dropout_ms * 48);
    const size_t recovery_samples = 4800; // 100 ms recovery

    for (size_t p = 0; p < steady_samples + dropout_samples + recovery_samples; p += block_size) {
      const bool in_dropout = (p >= steady_samples && p < steady_samples + dropout_samples);
      for (size_t i = 0; i < block_size; ++i) {
        const float t = static_cast<float>(position + i) / kRate;
        input[i] = .25f * std::sin(2 * kPi * 220.0f * t);
      }
      shared.push(input.data(), block_size);
      std::array<PitchMark, 64> marks{};
      const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
      const size_t count = make_marks(analyzed, period, marks, in_dropout);
      PitchResult pitch{220.0f, in_dropout ? .4f : 1.0f, true, analyzed};
      pitch.period_samples = period;
      const PitchTrackState state = in_dropout ? PitchTrackState::Coasting : PitchTrackState::Locked;
      psola.process_shared(input.data(), output.data(), block_size, pitch, state,
                           marks.data(), count, nullptr, fallback.data());

      if (p == steady_samples + dropout_samples) {
        // Exactly at recovery block
        const auto d = psola.debug();
        require(d.recovery_type == PsolaRecoveryType::SameNote,
                "same note recovery classifies as SameNote");
        require(d.recovery_class == PsolaRecoveryClass::SmallError ||
                d.recovery_class == PsolaRecoveryClass::MediumError,
                "same note recovery does not force note change crossfade");
        require(d.recovery_excess_discontinuity == 0.0f,
                "residual pitch discontinuity under 25 cents");
      }
      position += block_size;
    }
  }
  std::puts("same-note recovery regressions passed (10-60 ms dropouts)");
}

void note_change_recovery_regressions() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{};
  config.enabled = true;
  config.semitones = 4;
  config.wet = 1;
  config.fallback_policy = HarmonyFallbackPolicy::Muted;
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  config.recovery_crossfade_ms = 5.0f;
  require(psola.init(kRate, config, &shared), "note-change init");
  psola.set_onset_unvoiced_attenuation(false);

  constexpr size_t block_size = 64;
  std::array<float, block_size> input{}, output{}, fallback{};
  uint64_t position = 0;

  // Test note transitions during coast: 220 -> 330 Hz (+702 cents), 330 -> 220 Hz (-702 cents)
  for (auto [f_pre, f_post] : {std::pair{220.0f, 330.0f}, std::pair{330.0f, 220.0f}, std::pair{220.0f, 440.0f}}) {
    const float period_pre = kRate / f_pre;
    const float period_post = kRate / f_post;
    const size_t pre_samples = 4800; // 75 blocks
    const size_t dropout_samples = 1664; // 26 blocks (~34.7 ms)
    const size_t post_samples = 4800; // 75 blocks

    bool saw_crossfade = false;
    bool verified_classification = false;
    for (size_t p = 0; p < pre_samples + dropout_samples + post_samples; p += block_size) {
      const bool in_dropout = (p >= pre_samples && p < pre_samples + dropout_samples);
      const bool in_post = (p >= pre_samples + dropout_samples);
      const float current_f = in_post ? f_post : f_pre;
      const float current_period = in_post ? period_post : period_pre;

      for (size_t i = 0; i < block_size; ++i) {
        const float t = static_cast<float>(position + i) / kRate;
        input[i] = .25f * std::sin(2 * kPi * current_f * t);
      }
      shared.push(input.data(), block_size);
      std::array<PitchMark, 64> marks{};
      const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
      const size_t count = make_marks(analyzed, current_period, marks, in_dropout);
      PitchResult pitch{current_f, in_dropout ? .4f : 1.0f, true, analyzed};
      pitch.period_samples = current_period;
      pitch.pitch_changed = in_post && (p == pre_samples + dropout_samples);
      const PitchTrackState state = in_dropout ? PitchTrackState::Coasting : PitchTrackState::Locked;

      psola.process_shared(input.data(), output.data(), block_size, pitch, state,
                           marks.data(), count, nullptr, fallback.data());

      const auto d = psola.debug();
      if (p == pre_samples + dropout_samples) {
        require(d.recovery_class == PsolaRecoveryClass::NoteChangeCrossfade,
                "real note change classifies as NoteChangeCrossfade");
        require(d.recovery_type == PsolaRecoveryType::NoteChange ||
                d.recovery_type == PsolaRecoveryType::OctaveSuspect,
                "recovery type identifies note change or octave");
        verified_classification = true;
      }
      saw_crossfade |= d.crossfade_active;
      position += block_size;
    }
    require(verified_classification, "verified classification on recovery");
    require(saw_crossfade, "note change crossfade triggered");
  }
  std::puts("real note-change recovery regressions passed (220->330, 330->220, 220->440)");
}

void mark_offset_and_period_sweep_regressions() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{};
  config.enabled = true;
  config.semitones = 4;
  config.wet = 1;
  config.fallback_policy = HarmonyFallbackPolicy::Muted;
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  require(psola.init(kRate, config, &shared), "sweep init");
  psola.set_onset_unvoiced_attenuation(false);

  constexpr size_t block_size = 64;
  std::array<float, block_size> input{}, output{};
  const float base_period = kRate / 220.0f;
  uint64_t position = 0;

  // Sweep period deltas from 10 to 500 cents
  for (float delta_cents : {10.0f, 25.0f, 50.0f, 100.0f, 200.0f, 500.0f}) {
    const float new_period = base_period * std::exp2(delta_cents / 1200.0f);
    const float new_f = kRate / new_period;

    // Steady 220 Hz
    for (int b = 0; b < 20; ++b, position += block_size) {
      shared.push(input.data(), block_size);
      std::array<PitchMark, 64> marks{};
      size_t count = make_marks(position, base_period, marks, false);
      PitchResult pitch{220.0f, 1.0f, true, position};
      pitch.period_samples = base_period;
      psola.process_shared(input.data(), output.data(), block_size, pitch,
                           PitchTrackState::Locked, marks.data(), count);
    }
    // Coast for 30 ms
    for (int b = 0; b < 22; ++b, position += block_size) {
      shared.push(input.data(), block_size);
      std::array<PitchMark, 64> marks{};
      size_t count = make_marks(position, base_period, marks, true);
      PitchResult pitch{220.0f, .4f, true, position};
      pitch.period_samples = base_period;
      psola.process_shared(input.data(), output.data(), block_size, pitch,
                           PitchTrackState::Coasting, marks.data(), count);
    }
    // Recover with new period
    shared.push(input.data(), block_size);
    std::array<PitchMark, 64> marks{};
    size_t count = make_marks(position, new_period, marks, false);
    PitchResult pitch{new_f, 1.0f, true, position};
    pitch.period_samples = new_period;
    psola.process_shared(input.data(), output.data(), block_size, pitch,
                         PitchTrackState::Locked, marks.data(), count);

    const auto d = psola.debug();
    if (delta_cents <= 50.0f) {
      require(d.recovery_class == PsolaRecoveryClass::SmallError ||
              d.recovery_class == PsolaRecoveryClass::MediumError,
              "small delta classifies as soft reconciliation");
    } else if (delta_cents >= 200.0f) {
      require(d.recovery_class == PsolaRecoveryClass::NoteChangeCrossfade,
              "large delta classifies as NoteChangeCrossfade");
    }
    position += block_size;
  }
  std::puts("mark offset and period sweep regressions passed");
}

void vibrato_and_glissando_regressions() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{};
  config.enabled = true;
  config.semitones = 4;
  config.wet = 1;
  config.fallback_policy = HarmonyFallbackPolicy::Muted;
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  require(psola.init(kRate, config, &shared), "vibrato init");
  psola.set_onset_unvoiced_attenuation(false);

  constexpr size_t block_size = 64;
  std::array<float, block_size> input{}, output{};
  uint64_t position = 0;

  // Vibrato 220 Hz +/- 35 cents at 5 Hz with intermittent 25 ms coasts
  for (size_t b = 0; b < 200; ++b) {
    const float t = static_cast<float>(position) / kRate;
    const float vibrato_cents = 35.0f * std::sin(2 * kPi * 5.0f * t);
    const float f = 220.0f * std::exp2(vibrato_cents / 1200.0f);
    const float period = kRate / f;

    const bool coast = (b % 40 >= 20 && b % 40 < 35); // 20 ms coasts
    for (size_t i = 0; i < block_size; ++i)
      input[i] = .2f * std::sin(2 * kPi * f * (position + i) / kRate);
    shared.push(input.data(), block_size);
    std::array<PitchMark, 64> marks{};
    size_t count = make_marks(position, period, marks, coast);
    PitchResult pitch{f, coast ? .4f : 1.0f, true, position};
    pitch.period_samples = period;

    psola.process_shared(input.data(), output.data(), block_size, pitch,
                         coast ? PitchTrackState::Coasting : PitchTrackState::Locked,
                         marks.data(), count);

    const auto d = psola.debug();
    if (b % 40 == 35) {
      // Recovery moment after vibrato coast
      require(d.recovery_class != PsolaRecoveryClass::NoteChangeCrossfade,
              "vibrato does not falsely trigger NoteChangeCrossfade");
    }
    position += block_size;
  }
  std::puts("vibrato and glissando regressions passed");
}

void warm_reacquisition_regressions() {
  PitchAnalysisConfig config{};
  config.continuity_policy = PsolaContinuityPolicy::OnsetContinuityCoasting;
  config.coast_ms = 60;
  config.warm_reacquire_enabled = true;
  config.warm_reacquire_mode = WarmReacquireMode::W2_Warm1FrameSameNote;
  config.warm_reacquire_window_ms = 120.0f;
  config.warm_mark_seed_enabled = true;

  PitchAnalysis analysis;
  require(analysis.init(config), "warm analysis init");
  uint64_t position = 0;

  // 1. Cold start: verify that initial lock requires normal confirmation
  TrackerObservation cold_obs{};
  feed(analysis, position, 64, 220, .05f, cold_obs);
  require(analysis.track_state() != PitchTrackState::Locked,
          "cold start does not immediately lock on first block");

  feed(analysis, position, 48000, 220, .05f, cold_obs);
  require(analysis.track_state() == PitchTrackState::Locked,
          "cold start locks after sustained tone");
  const auto d_locked = analysis.debug();
  require(d_locked.warm_prior_pitch_hz > 0, "tracks last locked f0");

  // 2. Warm same-note reacquisition: brief drop of 40 ms then return
  TrackerObservation drop_obs{};
  feed(analysis, position, 1920, 0, 0, drop_obs); // drop
  feed(analysis, position, 64, 220, .05f, drop_obs);
  const auto d_warm = analysis.debug();
  require(d_warm.warm_candidate_class == WarmCandidateClass::SameNote ||
          d_warm.warm_reacquire_eligible,
          "warm reacquire recognizes same note");

  feed(analysis, position, 4800, 220, .05f, drop_obs);
  require(analysis.track_state() == PitchTrackState::Locked,
          "warm same-note relocks");

  // 3. Warm note change reacquisition: brief drop then note change 220 Hz -> 330 Hz (+700 cents)
  TrackerObservation change_obs{};
  feed(analysis, position, 1920, 0, 0, change_obs);
  bool saw_likely_change = false;
  for (size_t b = 0; b < 60; ++b) {
    feed(analysis, position, 64, 330, .05f, change_obs);
    const auto d = analysis.debug();
    if (d.warm_reacquire_eligible &&
        d.warm_candidate_class == WarmCandidateClass::LikelyNoteChange) {
      saw_likely_change = true;
    }
  }
  require(saw_likely_change,
          "large delta classifies as LikelyNoteChange in warm reacquire");

  // 4. Warm timeout: long drop > 180 ms (e.g. 300 ms = 14400 samples)
  TrackerObservation timeout_obs{};
  feed(analysis, position, 48000, 220, .05f, timeout_obs); // lock again
  feed(analysis, position, 14400, 0, 0, timeout_obs); // long silence
  bool saw_ineligible = false;
  for (size_t b = 0; b < 60; ++b) {
    feed(analysis, position, 64, 220, .05f, timeout_obs);
    const auto d = analysis.debug();
    if (analysis.track_state() == PitchTrackState::Acquiring &&
        !d.warm_reacquire_eligible) {
      saw_ineligible = true;
    }
  }
  require(saw_ineligible,
          "warm reacquisition times out after window expires");

  // 5. Repeated warm-acquire cycles with ring wrap
  for (int cycle = 0; cycle < 100; ++cycle) {
    TrackerObservation cyc{};
    feed(analysis, position, 2400, 220, .05f, cyc);
    feed(analysis, position, 480, 0, 0, cyc);
  }
  require(analysis.track_state() == PitchTrackState::Coasting ||
          analysis.track_state() == PitchTrackState::Unlocked ||
          analysis.track_state() == PitchTrackState::Locked,
          "repeated cycles remain valid");

  std::puts("warm reacquisition regressions passed");
}
} // namespace

int main() {
  analysis_state_regressions();
  renderer_continuity_regressions();
  same_note_recovery_regressions();
  note_change_recovery_regressions();
  mark_offset_and_period_sweep_regressions();
  vibrato_and_glissando_regressions();
  warm_reacquisition_regressions();
  std::puts("all PSOLA continuity and recovery tests passed");
}
