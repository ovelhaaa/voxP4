#pragma once

#include "audio_i2s.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring-Up Runners for Alpha 0.1B
void run_i2s_stage_b1_dac_bringup(void);
void run_i2s_stage_b2_adc_bringup(void);
void run_i2s_stage_b3_bypass_stress(void);
void run_i2s_stage_b4_full_dsp(void);
void run_i2s_stage_b4a_quiescent(void);
void run_i2s_stage_b4b_synthetic_voiced(void);
void run_i2s_stage_b4c_real_analog(void);
void run_i2s_latency_pulse_test(void);
void run_i2s_sample_format_forensic(void);

// Generic runner dispatching based on Kconfig mode
void run_i2s_bringup_selected_mode(void);

#ifdef __cplusplus
}
#endif
