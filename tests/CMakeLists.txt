cmake_minimum_required(VERSION 3.16)
project(vocal_fx_host_tests CXX)
enable_testing()
set(VF ../components/vocal_fx)
set(VOXLINK ../components/voxlink)
add_library(voxlink_core
 ${VOXLINK}/src/voxlink_codec.cpp
 ${VOXLINK}/src/voxlink_crc.cpp
 ${VOXLINK}/src/voxlink_registry.cpp
 ${VOXLINK}/src/voxlink_state.cpp
 ${VOXLINK}/src/voxlink_session.cpp)
target_include_directories(voxlink_core PUBLIC ${VOXLINK}/include)
target_compile_features(voxlink_core PUBLIC cxx_std_17)
target_compile_options(voxlink_core PRIVATE -Wall -Wextra -Werror)
add_library(vocal_fx_dsp
 ${VF}/dsp/biquad.cpp ${VF}/dsp/smoothing.cpp ${VF}/dsp/limiter.cpp ${VF}/dsp/tempo.cpp
 ${VF}/dynamics/gate.cpp ${VF}/dynamics/compressor.cpp ${VF}/delay/delay.cpp
 ${VF}/src/vocal_fx.cpp ${VF}/reverb/diffuser.cpp ${VF}/reverb/fdn_reverb.cpp
 ${VF}/platform/profiling.cpp ${VF}/platform/audio_i2s.cpp)
target_sources(vocal_fx_dsp PRIVATE ${VF}/pitch/decimator.cpp
 ${VF}/pitch/yin_detector.cpp ${VF}/pitch/pitch_mark_ncc.cpp ${VF}/pitch/pitch_analysis.cpp ${VF}/shift/td_psola.cpp
 ${VF}/formant/lpc.cpp
 ${VF}/harmony/scale.cpp ${VF}/harmony/midi_harmony.cpp ${VF}/harmony/harmony_engine.cpp ${VF}/harmony/harmony_articulation_envelope.cpp ${VF}/harmony/harmony_unvoiced_articulation.cpp ${VF}/harmony/harmony_plosive_bridge.cpp)
target_include_directories(vocal_fx_dsp PUBLIC ${VF}/include ${VF}/src ${VF}/dsp ${VF}/dynamics ${VF}/delay ${VF}/reverb ${VF}/platform ${VF}/pitch ${VF}/shift ${VF}/harmony ${VF}/formant)
target_compile_features(vocal_fx_dsp PUBLIC cxx_std_17)
target_compile_options(vocal_fx_dsp PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vocal_fx_dsp PRIVATE VOCAL_FX_ENABLE_PROFILING=1)
add_executable(dsp_tests test_dsp.cpp)
target_link_libraries(dsp_tests PRIVATE vocal_fx_dsp)
add_test(NAME dsp_tests COMMAND dsp_tests)
add_executable(test_tempo test_tempo.cpp)
target_link_libraries(test_tempo PRIVATE vocal_fx_dsp)
add_test(NAME test_tempo COMMAND test_tempo)
add_executable(pitch_tests test_pitch.cpp)
target_link_libraries(pitch_tests PRIVATE vocal_fx_dsp)
add_test(NAME pitch_tests COMMAND pitch_tests)
add_executable(yin_kernel_audit yin_kernel_audit.cpp)
target_link_libraries(yin_kernel_audit PRIVATE vocal_fx_dsp)
target_compile_definitions(yin_kernel_audit PRIVATE
 VOXP4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME yin_kernel_audit COMMAND yin_kernel_audit --verify)
add_executable(yin_cmnd_audit yin_cmnd_audit.cpp)
target_link_libraries(yin_cmnd_audit PRIVATE vocal_fx_dsp)
target_compile_definitions(yin_cmnd_audit PRIVATE
 VOXP4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME yin_cmnd_audit COMMAND yin_cmnd_audit --verify)
add_executable(yin_energy_audit yin_energy_audit.cpp)
target_link_libraries(yin_energy_audit PRIVATE vocal_fx_dsp)
target_compile_definitions(yin_energy_audit PRIVATE
 VOXP4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME yin_energy_audit COMMAND yin_energy_audit --verify)
add_executable(p4_production_defaults test_p4_production_defaults.cpp)
target_link_libraries(p4_production_defaults PRIVATE vocal_fx_dsp)
add_test(NAME p4_production_defaults COMMAND p4_production_defaults)
add_executable(yin_incremental_tests test_yin_incremental.cpp)
target_link_libraries(yin_incremental_tests PRIVATE vocal_fx_dsp)
add_test(NAME yin_incremental_tests COMMAND yin_incremental_tests)
add_executable(pitch_mark_ncc_audit pitch_mark_ncc_audit.cpp)
target_link_libraries(pitch_mark_ncc_audit PRIVATE vocal_fx_dsp)
target_compile_definitions(pitch_mark_ncc_audit PRIVATE
 VOXP4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME pitch_mark_ncc_audit COMMAND pitch_mark_ncc_audit --verify)
add_executable(psola_tests test_psola.cpp)
target_link_libraries(psola_tests PRIVATE vocal_fx_dsp)
add_test(NAME psola_tests COMMAND psola_tests)
add_executable(psola_kernel_tests test_psola_kernels.cpp)
target_link_libraries(psola_kernel_tests PRIVATE vocal_fx_dsp)
add_test(NAME psola_kernel_tests COMMAND psola_kernel_tests)
add_executable(render_wav_compare render_wav_compare.cpp)
target_link_libraries(render_wav_compare PRIVATE vocal_fx_dsp)
add_test(NAME render_wav_compare COMMAND render_wav_compare "${CMAKE_CURRENT_SOURCE_DIR}/../samples/dry-acapella-leave-this-place_95bpm.wav")
add_executable(unvoiced_articulation_tests test_unvoiced_articulation.cpp)
target_link_libraries(unvoiced_articulation_tests PRIVATE vocal_fx_dsp)
add_test(NAME unvoiced_articulation_tests COMMAND unvoiced_articulation_tests)
add_executable(plosive_bridge_tests test_plosive_bridge.cpp)
target_link_libraries(plosive_bridge_tests PRIVATE vocal_fx_dsp)
add_test(NAME plosive_bridge_tests COMMAND plosive_bridge_tests)
add_executable(lpc_tests test_lpc.cpp)
target_link_libraries(lpc_tests PRIVATE vocal_fx_dsp)
add_test(NAME lpc_tests COMMAND lpc_tests)
add_executable(lpc_ring_tests test_lpc_ring.cpp)
target_link_libraries(lpc_ring_tests PRIVATE vocal_fx_dsp)
add_test(NAME lpc_ring_tests COMMAND lpc_ring_tests)
add_executable(lpc_autocorr_audit lpc_autocorr_audit.cpp)
target_link_libraries(lpc_autocorr_audit PRIVATE vocal_fx_dsp)
add_test(NAME lpc_autocorr_audit COMMAND lpc_autocorr_audit)
add_executable(lpc_compensated_autocorr_audit lpc_compensated_autocorr_audit.cpp)
target_link_libraries(lpc_compensated_autocorr_audit PRIVATE vocal_fx_dsp)
add_test(NAME lpc_compensated_autocorr_audit COMMAND lpc_compensated_autocorr_audit)
add_executable(lpc_windowing_audit lpc_windowing_audit.cpp)
target_link_libraries(lpc_windowing_audit PRIVATE vocal_fx_dsp)
target_compile_definitions(lpc_windowing_audit PRIVATE
 VOXP4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME lpc_windowing_audit COMMAND lpc_windowing_audit)
add_executable(formant_stabilization_tests test_formant_stabilization.cpp)
target_link_libraries(formant_stabilization_tests PRIVATE vocal_fx_dsp)
add_test(NAME formant_stabilization_tests COMMAND formant_stabilization_tests)
add_executable(gainnorm_render_equivalence_tests test_gainnorm_render_equivalence.cpp)
target_link_libraries(gainnorm_render_equivalence_tests PRIVATE vocal_fx_dsp)
add_test(NAME gainnorm_render_equivalence_tests COMMAND gainnorm_render_equivalence_tests)
add_executable(harmony_tests test_harmony.cpp)
target_link_libraries(harmony_tests PRIVATE vocal_fx_dsp)
add_test(NAME harmony_tests COMMAND harmony_tests)
add_executable(test_harmony_timing test_harmony_timing.cpp)
target_link_libraries(test_harmony_timing PRIVATE vocal_fx_dsp)
add_test(NAME test_harmony_timing COMMAND test_harmony_timing)
add_executable(fdn_ir generate_fdn_ir.cpp)
target_link_libraries(fdn_ir PRIVATE vocal_fx_dsp)
add_executable(pipeline_benchmark benchmark.cpp)
target_link_libraries(pipeline_benchmark PRIVATE vocal_fx_dsp)
add_executable(pitch_benchmark pitch_benchmark.cpp)
target_link_libraries(pitch_benchmark PRIVATE vocal_fx_dsp)
add_executable(pitch_analyze pitch_analyze.cpp)
target_link_libraries(pitch_analyze PRIVATE vocal_fx_dsp)
add_executable(pitch_shift pitch_shift.cpp)
target_link_libraries(pitch_shift PRIVATE vocal_fx_dsp)
add_executable(psola_benchmark psola_benchmark.cpp)
target_link_libraries(psola_benchmark PRIVATE vocal_fx_dsp)
add_executable(ola_energy_probe ola_energy_probe.cpp)
target_link_libraries(ola_energy_probe PRIVATE vocal_fx_dsp)
add_executable(formant_benchmark formant_benchmark.cpp)
target_link_libraries(formant_benchmark PRIVATE vocal_fx_dsp)
add_executable(m512_render m512_render.cpp)
target_link_libraries(m512_render PRIVATE vocal_fx_dsp)
add_test(NAME psola_benchmark_rejects_text COMMAND psola_benchmark nope)
set_tests_properties(psola_benchmark_rejects_text PROPERTIES WILL_FAIL TRUE)
add_test(NAME psola_benchmark_rejects_zero COMMAND psola_benchmark 0)
set_tests_properties(psola_benchmark_rejects_zero PROPERTIES WILL_FAIL TRUE)
add_executable(host_headless_compare host_headless_compare.cpp)
target_link_libraries(host_headless_compare PRIVATE vocal_fx_dsp)
add_test(NAME psola_benchmark_rejects_overflow COMMAND psola_benchmark 44740)
set_tests_properties(psola_benchmark_rejects_overflow PROPERTIES WILL_FAIL TRUE)

add_executable(test_audio_transport test_audio_transport.cpp)
target_link_libraries(test_audio_transport PRIVATE vocal_fx_dsp)
add_test(NAME test_audio_transport COMMAND test_audio_transport)
add_executable(b4c6a_loop_timing_tests test_b4c6a_loop_timing.cpp)
target_link_libraries(b4c6a_loop_timing_tests PRIVATE vocal_fx_dsp)
add_test(NAME b4c6a_loop_timing_tests COMMAND b4c6a_loop_timing_tests)
add_executable(b4c7_reuse_tests test_b4c7_reuse.cpp)
target_link_libraries(b4c7_reuse_tests PRIVATE vocal_fx_dsp)
add_test(NAME b4c7_reuse_tests COMMAND b4c7_reuse_tests)
add_executable(b4c7_deferred_cache_tests test_b4c7_deferred_cache_equivalence.cpp)
target_link_libraries(b4c7_deferred_cache_tests PRIVATE vocal_fx_dsp)
add_test(NAME b4c7_deferred_cache_tests COMMAND b4c7_deferred_cache_tests)

# B4D.2 exact reverb block-path equivalence.
add_executable(reverb_equivalence_tests test_reverb_equivalence.cpp)
target_link_libraries(reverb_equivalence_tests PRIVATE vocal_fx_dsp)
add_test(NAME reverb_equivalence_tests COMMAND reverb_equivalence_tests)

# B4D.3 compressor/delay/GainNorm exact equivalence.
add_executable(b4d3_equivalence_tests test_b4d3_equivalence.cpp)
target_link_libraries(b4d3_equivalence_tests PRIVATE vocal_fx_dsp)
add_test(NAME b4d3_equivalence_tests COMMAND b4d3_equivalence_tests)

# B4D.3S sample-rate listening renders (manual tool, not a ctest).
add_executable(b4d3s_render b4d3s_render.cpp)
target_link_libraries(b4d3s_render PRIVATE vocal_fx_dsp)

# B4D.5 exact mark-selection equivalence gate and isolated OLA A/B benchmark.
add_executable(b4d5_mark_equivalence_tests test_b4d5_mark_equivalence.cpp)
target_link_libraries(b4d5_mark_equivalence_tests PRIVATE vocal_fx_dsp)
add_test(NAME b4d5_mark_equivalence_tests COMMAND b4d5_mark_equivalence_tests)
add_executable(b4d5_ola_ab b4d5_ola_ab.cpp)
target_link_libraries(b4d5_ola_ab PRIVATE vocal_fx_dsp)
add_test(NAME b4d5_ola_ab COMMAND b4d5_ola_ab)
add_executable(b4d5_model_equivalence_tests test_b4d5_model_equivalence.cpp)
target_link_libraries(b4d5_model_equivalence_tests PRIVATE vocal_fx_dsp)
add_test(NAME b4d5_model_equivalence_tests COMMAND b4d5_model_equivalence_tests)

# VoxLink control-plane host tests.
add_executable(voxlink_registry_tests test_voxlink_registry.cpp)
target_link_libraries(voxlink_registry_tests PRIVATE voxlink_core)
target_include_directories(voxlink_registry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME voxlink_registry_tests COMMAND voxlink_registry_tests)

add_executable(voxlink_schema_tests test_voxlink_schema.cpp)
target_link_libraries(voxlink_schema_tests PRIVATE voxlink_core)
target_compile_definitions(voxlink_schema_tests PRIVATE
 VOXP4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME voxlink_schema_tests COMMAND voxlink_schema_tests)

add_executable(voxlink_framing_tests test_voxlink_framing.cpp)
target_link_libraries(voxlink_framing_tests PRIVATE voxlink_core)
add_test(NAME voxlink_framing_tests COMMAND voxlink_framing_tests)

add_executable(voxlink_fuzz_tests test_voxlink_fuzz.cpp)
target_link_libraries(voxlink_fuzz_tests PRIVATE voxlink_core)
add_test(NAME voxlink_fuzz_tests COMMAND voxlink_fuzz_tests)

add_executable(voxlink_state_sync_tests test_voxlink_state_sync.cpp)
target_link_libraries(voxlink_state_sync_tests PRIVATE voxlink_core)
add_test(NAME voxlink_state_sync_tests COMMAND voxlink_state_sync_tests)

find_package(Threads REQUIRED)
add_executable(voxlink_concurrency_tests test_voxlink_concurrency.cpp)
target_link_libraries(voxlink_concurrency_tests PRIVATE voxlink_core Threads::Threads)
target_include_directories(voxlink_concurrency_tests PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR} ${VF}/src ${VF}/include)
add_test(NAME voxlink_concurrency_tests COMMAND voxlink_concurrency_tests)

add_executable(voxlink_golden_tests test_voxlink_golden.cpp)
target_link_libraries(voxlink_golden_tests PRIVATE voxlink_core)
target_include_directories(voxlink_golden_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME voxlink_golden_tests COMMAND voxlink_golden_tests)

add_executable(voxlink_binding_tests
 test_voxlink_binding.cpp ${VF}/control/vocal_fx_param_binding.cpp)
target_link_libraries(voxlink_binding_tests PRIVATE vocal_fx_dsp voxlink_core)
target_include_directories(voxlink_binding_tests PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR} ${VF}/control)
add_test(NAME voxlink_binding_tests COMMAND voxlink_binding_tests)

add_executable(voxlink_coherence_tests
 test_voxlink_coherence.cpp ${VF}/control/vocal_fx_param_binding.cpp)
target_link_libraries(voxlink_coherence_tests PRIVATE vocal_fx_dsp voxlink_core)
target_include_directories(voxlink_coherence_tests PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR} ${VF}/control)
add_test(NAME voxlink_coherence_tests COMMAND voxlink_coherence_tests)

add_executable(voxlink_protocol_conformance_tests test_voxlink_conformance.cpp)
target_link_libraries(voxlink_protocol_conformance_tests PRIVATE voxlink_core)
target_include_directories(voxlink_protocol_conformance_tests PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME voxlink_protocol_conformance_tests
 COMMAND voxlink_protocol_conformance_tests)
