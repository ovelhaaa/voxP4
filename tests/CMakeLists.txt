cmake_minimum_required(VERSION 3.16)
project(vocal_fx_host_tests CXX)
enable_testing()
set(VF ../components/vocal_fx)
add_library(vocal_fx_dsp
 ${VF}/dsp/biquad.cpp ${VF}/dsp/smoothing.cpp ${VF}/dsp/limiter.cpp
 ${VF}/dynamics/gate.cpp ${VF}/dynamics/compressor.cpp ${VF}/delay/delay.cpp
 ${VF}/src/vocal_fx.cpp ${VF}/reverb/diffuser.cpp ${VF}/reverb/fdn_reverb.cpp ${VF}/platform/profiling.cpp)
target_include_directories(vocal_fx_dsp PUBLIC ${VF}/include ${VF}/dsp ${VF}/dynamics ${VF}/delay ${VF}/reverb ${VF}/platform)
target_compile_features(vocal_fx_dsp PUBLIC cxx_std_17)
target_compile_options(vocal_fx_dsp PRIVATE -Wall -Wextra -Werror)
add_executable(dsp_tests test_dsp.cpp)
target_link_libraries(dsp_tests PRIVATE vocal_fx_dsp)
add_test(NAME dsp_tests COMMAND dsp_tests)
add_executable(fdn_ir generate_fdn_ir.cpp)
target_link_libraries(fdn_ir PRIVATE vocal_fx_dsp)
add_executable(pipeline_benchmark benchmark.cpp)
target_link_libraries(pipeline_benchmark PRIVATE vocal_fx_dsp)
