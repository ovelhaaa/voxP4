set(VOXLINK_SRCS
    src/voxlink_codec.cpp
    src/voxlink_crc.cpp
    src/voxlink_registry.cpp
    src/voxlink_state.cpp
    src/voxlink_session.cpp)
if(ESP_PLATFORM)
  list(APPEND VOXLINK_SRCS src/voxlink_uart.cpp)
endif()
idf_component_register(SRCS ${VOXLINK_SRCS} INCLUDE_DIRS include
    REQUIRES esp_driver_uart)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror
    -Wno-missing-field-initializers)
