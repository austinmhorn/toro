if(NOT DEFINED TORO_EXECUTABLE OR NOT DEFINED TORO_BINARY_DIR)
    message(FATAL_ERROR "PerformanceContractCliTest requires toro paths")
endif()

set(safe_source "${TORO_BINARY_DIR}/performance-safe-cli.toro")
set(blocked_source "${TORO_BINARY_DIR}/performance-blocked-cli.toro")
set(unknown_source "${TORO_BINARY_DIR}/performance-unknown-cli.toro")

file(WRITE "${safe_source}"
    "performance function calculate(value: int) -> int { return value + 1 }\n")
file(WRITE "${blocked_source}"
    "function output() { print(1) }\n"
    "performance function tick() { output() }\n")
file(WRITE "${unknown_source}"
    "extern \"C\" function mystery() -> c_int\n"
    "performance function tick() { value := mystery() }\n")

execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${safe_source}"
    RESULT_VARIABLE safe_result
    OUTPUT_VARIABLE safe_output
    ERROR_VARIABLE safe_error
)
if(NOT safe_result EQUAL 0 OR NOT safe_output STREQUAL "check passed\n")
    message(FATAL_ERROR "safe performance contract failed: ${safe_error}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${blocked_source}"
    RESULT_VARIABLE blocked_result
    OUTPUT_VARIABLE blocked_output
    ERROR_VARIABLE blocked_error
)
execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${blocked_source}"
    RESULT_VARIABLE repeated_result
    OUTPUT_VARIABLE repeated_output
    ERROR_VARIABLE repeated_error
)
if(blocked_result EQUAL 0 OR repeated_result EQUAL 0)
    message(FATAL_ERROR "blocking performance contract was accepted")
endif()
if(NOT blocked_error STREQUAL repeated_error)
    message(FATAL_ERROR "performance diagnostics were not deterministic")
endif()
if(NOT blocked_error MATCHES "violates its contract"
    OR NOT blocked_error MATCHES "blocking operation"
    OR NOT blocked_error MATCHES "call to 'output'"
    OR NOT blocked_error MATCHES "\\[performance-contract\\]")
    message(FATAL_ERROR "blocking diagnostic was incomplete: ${blocked_error}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${unknown_source}"
    RESULT_VARIABLE unknown_result
    OUTPUT_VARIABLE unknown_output
    ERROR_VARIABLE unknown_error
)
if(unknown_result EQUAL 0
    OR NOT unknown_error MATCHES "cannot be proven"
    OR NOT unknown_error MATCHES "unknown external behavior")
    message(FATAL_ERROR "unknown behavior diagnostic was incomplete: ${unknown_error}")
endif()

file(REMOVE "${safe_source}" "${blocked_source}" "${unknown_source}")
