set(B4B6_PRIVATE_INCLUDES "")
if(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION OR CONFIG_VOXP4_MODE_I2S_B4C_1_AUDIO_CORE_AUDIT OR CONFIG_VOXP4_MODE_I2S_B4C_2_AUDIO_CORE_AUDIT OR CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT OR CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT OR CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST OR CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED OR CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE OR CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS OR CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT OR CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC OR CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION OR CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
  set(B4B6_FIXTURE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/b4b6_vocal_fixture.h")
  execute_process(
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/prepare_b4b6_vocal_fixture.py"
            "${CMAKE_SOURCE_DIR}/samples/dry-acapella-leave-this-place_95bpm.wav"
            "${B4B6_FIXTURE_HEADER}"
    RESULT_VARIABLE B4B6_FIXTURE_RESULT)
  if(NOT B4B6_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to prepare the B4B.6 vocal fixture")
  endif()
  set(B4B6_PRIVATE_INCLUDES "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# The headless harness (19 KB wavetable, outlier rings, stress tasks) never
# runs in I2S bring-up modes but its statics consumed internal .bss; the
# B4C.7 link budget (single 393 KB engine section vs 384 KB sram_high) proved
# every kilobyte matters. Build it only for headless targets.
if(CONFIG_VOXP4_HEADLESS_HW_TEST)
  set(VOXP4_MAIN_SRCS "app_main.cpp" "headless_test.cpp" "i2s_bringup.cpp")
else()
  set(VOXP4_MAIN_SRCS "app_main.cpp" "i2s_bringup.cpp")
endif()
idf_component_register(SRCS ${VOXP4_MAIN_SRCS}
                       PRIV_INCLUDE_DIRS ${B4B6_PRIVATE_INCLUDES}
                       REQUIRES vocal_fx esp_timer)
# Same gp-window rationale as vocal_fx (see above): app small globals use
# absolute addressing; only IDF smalls remain gp-relative.
target_compile_options(${COMPONENT_LIB} PRIVATE -msmall-data-limit=0)


