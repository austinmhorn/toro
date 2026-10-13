cmake_minimum_required(VERSION 3.25)

project(toro_cmake_integration LANGUAGES C CXX)

if(NOT TORO_INTEGRATION_MODULE_DIR)
    set(TORO_INTEGRATION_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
endif()
list(APPEND CMAKE_MODULE_PATH "${TORO_INTEGRATION_MODULE_DIR}")
include(ToroIntegration)

toro_add_target(
    NAME game_logic
    PROJECT "${CMAKE_CURRENT_LIST_DIR}/toro"
)

add_executable(toro_mixed_native cpp/native.cpp)
target_link_libraries(toro_mixed_native PRIVATE game_logic)
