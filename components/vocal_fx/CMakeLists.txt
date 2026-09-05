set(SRCS src/vocal_fx.cpp dsp/biquad.cpp dsp/smoothing.cpp dsp/limiter.cpp
 dynamics/gate.cpp dynamics/compressor.cpp delay/delay.cpp
 reverb/diffuser.cpp reverb/fdn_reverb.cpp platform/profiling.cpp platform/audio_i2s.cpp)
idf_component_register(SRCS ${SRCS} INCLUDE_DIRS include dsp dynamics delay reverb platform REQUIRES esp_driver_i2s esp_timer esp_system)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VOCAL_FX_ENABLE_PROFILING=$<BOOL:${CONFIG_VOCAL_FX_ENABLE_PROFILING}>)
