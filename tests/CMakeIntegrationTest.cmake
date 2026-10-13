if(NOT DEFINED TORO_SOURCE_DIR OR NOT DEFINED TORO_BINARY_DIR
    OR NOT DEFINED TORO_EXECUTABLE)
    message(FATAL_ERROR "missing Toro CMake integration test configuration")
endif()

function(run_checked description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT status EQUAL 0)
        message(FATAL_ERROR
            "${description} failed (${status})\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

set(test_root "${TORO_BINARY_DIR}/cmake integration test")
set(test_source "${test_root}/source tree")
set(test_build "${test_root}/build tree")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_source}")
file(COPY "${TORO_SOURCE_DIR}/examples/cmake_integration/"
    DESTINATION "${test_source}")

run_checked("mixed-native configure"
    "${CMAKE_COMMAND}"
    -S "${test_source}"
    -B "${test_build}"
    "-DTORO_EXECUTABLE=${TORO_EXECUTABLE}"
    "-DTORO_INTEGRATION_MODULE_DIR=${TORO_SOURCE_DIR}/cmake")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${test_build}"
    RESULT_VARIABLE first_status
    OUTPUT_VARIABLE first_output
    ERROR_VARIABLE first_error
)
if(NOT first_status EQUAL 0)
    message(FATAL_ERROR
        "initial mixed-native build failed\n${first_output}\n${first_error}")
endif()
if(NOT first_output MATCHES "Generating C for Toro target game_logic")
    message(FATAL_ERROR "initial build did not generate the Toro target")
endif()

set(generated_c "${test_build}/toro-generated/game_logic/program.c")
set(generated_library "${test_build}/libgame_logic.a")
set(example_executable "${test_build}/toro_mixed_native")
foreach(expected IN ITEMS
    "${generated_c}" "${generated_library}" "${example_executable}")
    if(NOT EXISTS "${expected}")
        message(FATAL_ERROR "mixed-native build did not produce ${expected}")
    endif()
endforeach()

file(SHA256 "${generated_c}" initial_hash)
file(READ "${generated_c}" generated_contents)
string(FIND "${generated_contents}" "${TORO_SOURCE_DIR}" source_path_index)
if(NOT source_path_index EQUAL -1)
    message(FATAL_ERROR "generated C contains an absolute Toro source-tree path")
endif()
string(FIND "${generated_contents}" "${test_source}" project_path_index)
if(NOT project_path_index EQUAL -1)
    message(FATAL_ERROR "generated C contains an absolute project path")
endif()

run_checked("mixed-native executable"
    "${example_executable}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${test_build}"
    RESULT_VARIABLE unchanged_status
    OUTPUT_VARIABLE unchanged_output
    ERROR_VARIABLE unchanged_error
)
if(NOT unchanged_status EQUAL 0)
    message(FATAL_ERROR
        "unchanged rebuild failed\n${unchanged_output}\n${unchanged_error}")
endif()
if(unchanged_output MATCHES "Generating C for Toro target game_logic")
    message(FATAL_ERROR "unchanged build regenerated Toro C")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(APPEND "${test_source}/toro/src/main.toro" "\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${test_build}"
    RESULT_VARIABLE toro_change_status
    OUTPUT_VARIABLE toro_change_output
    ERROR_VARIABLE toro_change_error
)
if(NOT toro_change_status EQUAL 0
    OR NOT toro_change_output MATCHES "Generating C for Toro target game_logic")
    message(FATAL_ERROR
        "Toro source change did not regenerate output\n${toro_change_output}\n${toro_change_error}")
endif()
file(SHA256 "${generated_c}" repeated_hash)
if(NOT initial_hash STREQUAL repeated_hash)
    message(FATAL_ERROR "equivalent Toro input did not emit deterministic C")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(APPEND "${test_source}/cpp/native.cpp" "\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${test_build}"
    RESULT_VARIABLE cpp_change_status
    OUTPUT_VARIABLE cpp_change_output
    ERROR_VARIABLE cpp_change_error
)
if(NOT cpp_change_status EQUAL 0)
    message(FATAL_ERROR
        "C++ source rebuild failed\n${cpp_change_output}\n${cpp_change_error}")
endif()
if(cpp_change_output MATCHES "Generating C for Toro target game_logic")
    message(FATAL_ERROR "C++-only change regenerated Toro C")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(APPEND "${test_source}/toro/toro.toml" "\n# rebuild probe\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${test_build}"
    RESULT_VARIABLE manifest_status
    OUTPUT_VARIABLE manifest_output
    ERROR_VARIABLE manifest_error
)
if(NOT manifest_status EQUAL 0
    OR NOT manifest_output MATCHES "Generating C for Toro target game_logic")
    message(FATAL_ERROR
        "manifest change did not regenerate Toro output\n${manifest_output}\n${manifest_error}")
endif()

file(REMOVE_RECURSE "${test_build}")
run_checked("clean mixed-native reconfigure"
    "${CMAKE_COMMAND}"
    -S "${test_source}"
    -B "${test_build}"
    "-DTORO_EXECUTABLE=${TORO_EXECUTABLE}"
    "-DTORO_INTEGRATION_MODULE_DIR=${TORO_SOURCE_DIR}/cmake")
run_checked("clean mixed-native rebuild"
    "${CMAKE_COMMAND}" --build "${test_build}")
run_checked("clean mixed-native executable"
    "${test_build}/toro_mixed_native")
