#pragma once
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include <cstddef>

bool vocal_fx_init(const VocalFxConfig &config);
void vocal_fx_reset();
void vocal_fx_process(const float *input, float *output_l, float *output_r,
                      size_t frames);
void vocal_fx_set_parameter(VocalFxParameter parameter, float value);
void vocal_fx_publish_pitch(const PitchResult &result);
PitchResult vocal_fx_latest_pitch();
size_t vocal_fx_dsp_memory_bytes();
VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section);
