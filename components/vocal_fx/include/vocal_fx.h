#pragma once
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include <cstddef>

bool vocal_fx_init(const VocalFxConfig &config);
bool vocal_fx_init_pitch_analysis(const PitchAnalysisConfig &config);
void vocal_fx_reset();
void vocal_fx_process(const float *input, float *output_l, float *output_r,
                      size_t frames);
void vocal_fx_set_parameter(VocalFxParameter parameter, float value);
void vocal_fx_publish_pitch(const PitchResult &result);
PitchResult vocal_fx_latest_pitch();
// Called by the lower-priority analysis task; never by the audio callback.
size_t vocal_fx_run_pitch_analysis(size_t max_hops = 1);
bool vocal_fx_get_latest_pitch_mark(PitchMark *mark);
size_t vocal_fx_get_pitch_marks(uint64_t start_sample, uint64_t end_sample,
                                PitchMark *destination, size_t capacity);
PitchTrackState vocal_fx_pitch_track_state();
uint64_t vocal_fx_analysis_latency_samples();
VocalFxProfileStats
vocal_fx_pitch_profile_stats(PitchAnalysisProfileSection section);
size_t vocal_fx_dsp_memory_bytes();
VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section);
