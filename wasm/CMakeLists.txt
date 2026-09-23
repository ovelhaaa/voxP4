cmake_minimum_required(VERSION 3.16)
project(voxp4_wasm CXX)

# Resolve the real commit used to produce this WASM build. Falls back to
# "unknown" when Git is unavailable, never to a stale/incorrect commit.
find_package(Git QUIET)
set(VOXP4_GIT_COMMIT "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    OUTPUT_VARIABLE VOXP4_GIT_COMMIT_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE VOXP4_GIT_COMMIT_RESULT
  )
  if(VOXP4_GIT_COMMIT_RESULT EQUAL 0 AND VOXP4_GIT_COMMIT_OUTPUT)
    set(VOXP4_GIT_COMMIT "${VOXP4_GIT_COMMIT_OUTPUT}")
  endif()
endif()
message(STATUS "VoxP4 DSP commit: ${VOXP4_GIT_COMMIT}")

set(VF ${CMAKE_CURRENT_SOURCE_DIR}/../components/vocal_fx)
set(VOXLINK ${CMAKE_CURRENT_SOURCE_DIR}/../components/voxlink)

add_executable(voxp4-preview
  ${VF}/dsp/biquad.cpp
  ${VF}/dsp/smoothing.cpp
  ${VF}/dsp/limiter.cpp
  ${VF}/dsp/tempo.cpp
  ${VF}/dynamics/gate.cpp
  ${VF}/dynamics/compressor.cpp
  ${VF}/drive/vocal_drive.cpp
  ${VF}/delay/delay.cpp
  ${VF}/modulation/chorus.cpp
  ${VF}/src/vocal_fx.cpp
  ${VF}/reverb/diffuser.cpp
  ${VF}/reverb/fdn_reverb.cpp
  ${VF}/platform/profiling.cpp
  ${VF}/pitch/decimator.cpp
  ${VF}/pitch/yin_detector.cpp
  ${VF}/pitch/pitch_mark_ncc.cpp
  ${VF}/pitch/pitch_analysis.cpp
  ${VF}/shift/td_psola.cpp
  ${VF}/formant/lpc.cpp
  ${VF}/harmony/scale.cpp
  ${VF}/harmony/midi_harmony.cpp
  ${VF}/harmony/harmony_engine.cpp
  ${VF}/harmony/harmony_articulation_envelope.cpp
  ${VF}/harmony/harmony_unvoiced_articulation.cpp
  ${VF}/harmony/harmony_plosive_bridge.cpp
  ${VF}/control/vocal_fx_param_binding.cpp
  ${VOXLINK}/src/voxlink_registry.cpp
  voxp4_preview.cpp
)

target_include_directories(voxp4-preview PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${VF}/include
  ${VF}/src
  ${VF}/dsp
  ${VF}/dynamics
  ${VF}/drive
  ${VF}/delay
  ${VF}/modulation
  ${VF}/reverb
  ${VF}/platform
  ${VF}/pitch
  ${VF}/shift
  ${VF}/harmony
  ${VF}/formant
  ${VF}/control
  ${VOXLINK}/include
)

target_compile_features(voxp4-preview PRIVATE cxx_std_17)
target_compile_options(voxp4-preview PRIVATE -O3 -Wall -Wextra)
target_compile_definitions(voxp4-preview PRIVATE VOCAL_FX_ENABLE_PROFILING=0)
target_compile_definitions(voxp4-preview PRIVATE VOXP4_GIT_COMMIT="${VOXP4_GIT_COMMIT}")

if(EMSCRIPTEN)
  set_target_properties(voxp4-preview PROPERTIES
    OUTPUT_NAME "voxp4-preview"
    SUFFIX ".mjs"
  )

  target_link_options(voxp4-preview PRIVATE
    -O3
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sALLOW_MEMORY_GROWTH=1
    -sENVIRONMENT=web,worker
    "-sEXPORTED_FUNCTIONS=['_voxp4_preview_init','_voxp4_preview_set_parameter','_voxp4_preview_reset_parameters','_voxp4_preview_reset','_voxp4_preview_render','_voxp4_preview_version','_voxp4_preview_get_manifest_json','_voxp4_preview_tempo_subdivision_ms','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','UTF8ToString','stringToUTF8','HEAPF32','HEAPU8']"
  )
endif()
