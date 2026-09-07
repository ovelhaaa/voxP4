#pragma once
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include "harmony_engine.h"
#include "lpc.h"
#include <cstddef>

bool vocal_fx_init(const VocalFxConfig &config);
bool vocal_fx_init_pitch_analysis(const PitchAnalysisConfig &config);
void vocal_fx_reset();
void vocal_fx_process(const float *input, float *output_l, float *output_r,
                      size_t frames);
void vocal_fx_set_parameter(VocalFxParameter parameter, float value);
void vocal_fx_set_pitch_shift_enabled(bool enabled);
void vocal_fx_set_pitch_shift_semitones(float semitones);
void vocal_fx_set_pitch_shift_mix(float wet);
void vocal_fx_set_harmony_mode(HarmonyMode mode);
void vocal_fx_set_key(uint8_t chromatic_root);
void vocal_fx_set_scale(ScaleType scale);
void vocal_fx_set_harmony_enabled(size_t voice, bool enabled);
void vocal_fx_set_harmony_interval(size_t voice, float semitones);
void vocal_fx_set_harmony_degree(size_t voice, int degrees);
void vocal_fx_set_harmony_gain(size_t voice, float gain);
void vocal_fx_set_harmony_pan(size_t voice, float pan);
void vocal_fx_set_harmony_smoothing(size_t voice, float milliseconds);
void vocal_fx_set_formant_mode(size_t voice, FormantMode mode);
void vocal_fx_set_formant_amount(size_t voice, float amount);
void vocal_fx_midi_note_on(uint8_t note, uint8_t velocity);
void vocal_fx_midi_note_off(uint8_t note);
void vocal_fx_midi_all_notes_off();
PitchShiftTelemetry vocal_fx_harmony_telemetry(size_t voice);
uint32_t vocal_fx_pitch_shift_latency_samples();
PitchShiftTelemetry vocal_fx_pitch_shift_telemetry();
VocalFxProfileStats
vocal_fx_pitch_shift_profile_stats(PitchShiftProfileSection section);
void vocal_fx_publish_pitch(const PitchResult &result);
PitchResult vocal_fx_latest_pitch();
// Single-attempt snapshot for real-time callers. Returns false rather than
// waiting when a publisher is active or the snapshot changes during the read.
bool vocal_fx_try_latest_pitch(PitchResult *result);
// Called by the lower-priority analysis task; never by the audio callback.
size_t vocal_fx_run_pitch_analysis(size_t max_hops = 1);
bool vocal_fx_get_latest_pitch_mark(PitchMark *mark);
size_t vocal_fx_get_pitch_marks(uint64_t start_sample, uint64_t end_sample,
                                PitchMark *destination, size_t capacity);
bool vocal_fx_try_get_pitch_marks(uint64_t start_sample, uint64_t end_sample,
                                  PitchMark *destination, size_t capacity,
                                  size_t *written);
PitchTrackState vocal_fx_pitch_track_state();
uint64_t vocal_fx_analysis_latency_samples();
VocalFxProfileStats
vocal_fx_pitch_profile_stats(PitchAnalysisProfileSection section);
size_t vocal_fx_dsp_memory_bytes();
VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section);
LpcTelemetry vocal_fx_lpc_telemetry();
VocalFxProfileStats vocal_fx_lpc_profile_stats(LpcProfileSection section);
