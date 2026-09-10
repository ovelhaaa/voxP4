set(SRCS src/vocal_fx.cpp dsp/biquad.cpp dsp/smoothing.cpp dsp/limiter.cpp
 dynamics/gate.cpp dynamics/compressor.cpp delay/delay.cpp
 reverb/diffuser.cpp reverb/fdn_reverb.cpp platform/profiling.cpp platform/audio_i2s.cpp)
list(APPEND SRCS pitch/decimator.cpp pitch/yin_detector.cpp pitch/pitch_analysis.cpp)
list(APPEND SRCS shift/td_psola.cpp)
list(APPEND SRCS formant/lpc.cpp)
list(APPEND SRCS harmony/scale.cpp harmony/midi_harmony.cpp harmony/harmony_engine.cpp harmony/harmony_articulation_envelope.cpp harmony/harmony_unvoiced_articulation.cpp harmony/harmony_plosive_bridge.cpp)
idf_component_register(SRCS ${SRCS} INCLUDE_DIRS include dsp dynamics delay reverb platform pitch shift harmony formant REQUIRES esp_driver_i2s esp_timer esp_system)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror
    -Wno-missing-field-initializers)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VOCAL_FX_ENABLE_PROFILING=$<BOOL:${CONFIG_VOCAL_FX_ENABLE_PROFILING}>)
if(CONFIG_VOCAL_FX_PITCH_DEBUG)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE VOCAL_FX_PITCH_DEBUG=1)
endif()
