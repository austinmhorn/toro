if(NOT DEFINED TORO_EXECUTABLE OR NOT DEFINED TORO_BINARY_DIR)
    message(FATAL_ERROR "PerformanceContractCliTest requires toro paths")
endif()

set(safe_source "${TORO_BINARY_DIR}/performance-safe-cli.toro")
set(stack_source "${TORO_BINARY_DIR}/performance-stack-cli.toro")
set(heap_source "${TORO_BINARY_DIR}/performance-heap-cli.toro")
set(blocked_source "${TORO_BINARY_DIR}/performance-blocked-cli.toro")
set(unknown_source "${TORO_BINARY_DIR}/performance-unknown-cli.toro")

file(WRITE "${safe_source}"
    "performance function calculate(value: int) -> int { return value + 1 }\n")
file(WRITE "${stack_source}"
    "class Item {}\n"
    "performance function local() { value := Item() }\n")
file(WRITE "${heap_source}"
    "class Item {}\n"
    "performance function make() -> Item { return Item() }\n")
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
    COMMAND "${TORO_EXECUTABLE}" check "${stack_source}"
    RESULT_VARIABLE stack_result
    OUTPUT_VARIABLE stack_output
    ERROR_VARIABLE stack_error
)
if(NOT stack_result EQUAL 0 OR NOT stack_output STREQUAL "check passed\n")
    message(FATAL_ERROR "stack-backed performance contract failed: ${stack_error}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${heap_source}"
    RESULT_VARIABLE heap_result
    OUTPUT_VARIABLE heap_output
    ERROR_VARIABLE heap_error
)
if(heap_result EQUAL 0
    OR NOT heap_error MATCHES "general heap allocation"
    OR NOT heap_error MATCHES "escapes through return")
    message(FATAL_ERROR "heap-backed performance contract was accepted: ${heap_error}")
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
    OR NOT blocked_error MATCHES "'tick' calls 'output'"
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
    OR NOT unknown_error MATCHES "has no performance contract")
    message(FATAL_ERROR "unknown behavior diagnostic was incomplete: ${unknown_error}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" ir --performance "${stack_source}"
    RESULT_VARIABLE stack_report_result
    OUTPUT_VARIABLE stack_report
    ERROR_VARIABLE stack_report_error
)
if(NOT stack_report_result EQUAL 0
    OR NOT stack_report MATCHES "contract satisfied"
    OR NOT stack_report MATCHES "runtime-storage stack"
    OR NOT stack_report MATCHES "general-heap-allocation discharged")
    message(FATAL_ERROR "stack performance report was incomplete: ${stack_report_error}${stack_report}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" ir --performance "${heap_source}"
    RESULT_VARIABLE heap_report_result
    OUTPUT_VARIABLE heap_report
    ERROR_VARIABLE heap_report_error
)
if(NOT heap_report_result EQUAL 0
    OR NOT heap_report MATCHES "contract violated"
    OR NOT heap_report MATCHES "runtime-storage heap"
    OR NOT heap_report MATCHES "escapes through return")
    message(FATAL_ERROR "heap performance report was incomplete: ${heap_report_error}${heap_report}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" ir --performance "${unknown_source}"
    RESULT_VARIABLE unknown_report_result
    OUTPUT_VARIABLE unknown_report
    ERROR_VARIABLE unknown_report_error
)
if(NOT unknown_report_result EQUAL 0
    OR NOT unknown_report MATCHES "contract unproven"
    OR NOT unknown_report MATCHES "has no performance contract")
    message(FATAL_ERROR "unknown performance report was incomplete: ${unknown_report_error}${unknown_report}")
endif()

file(REMOVE "${safe_source}" "${stack_source}" "${heap_source}"
    "${blocked_source}" "${unknown_source}")
