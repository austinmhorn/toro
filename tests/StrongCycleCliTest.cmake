if(NOT DEFINED TORO_EXECUTABLE OR NOT DEFINED TORO_BINARY_DIR)
    message(FATAL_ERROR "StrongCycleCliTest requires toro paths")
endif()

set(cycle_source "${TORO_BINARY_DIR}/strong-cycle-cli.toro")
set(weak_source "${TORO_BINARY_DIR}/weak-cycle-cli.toro")

file(WRITE "${cycle_source}"
    "class Parent { public child: Child? }\n"
    "class Child { public parent: Parent? }\n")
file(WRITE "${weak_source}"
    "class Parent { public child: Child? }\n"
    "class Child { public weak parent: Parent? }\n")

execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${cycle_source}"
    RESULT_VARIABLE cycle_result
    OUTPUT_VARIABLE cycle_output
    ERROR_VARIABLE cycle_error
)
if(NOT cycle_result EQUAL 0)
    message(FATAL_ERROR "cycle warning rejected a valid program: ${cycle_error}")
endif()
if(NOT cycle_output STREQUAL "check passed\n")
    message(FATAL_ERROR "unexpected cycle check output: ${cycle_output}")
endif()
if(NOT cycle_error MATCHES "warning: possible strong reference cycle"
    OR NOT cycle_error MATCHES "\\[strong-reference-cycle\\]")
    message(FATAL_ERROR "cycle warning was not emitted: ${cycle_error}")
endif()
if(NOT cycle_error MATCHES "Parent.child -> Child"
    OR NOT cycle_error MATCHES "Child.parent -> Parent")
    message(FATAL_ERROR "cycle warning omitted field edges: ${cycle_error}")
endif()

execute_process(
    COMMAND "${TORO_EXECUTABLE}" check "${weak_source}"
    RESULT_VARIABLE weak_result
    OUTPUT_VARIABLE weak_output
    ERROR_VARIABLE weak_error
)
if(NOT weak_result EQUAL 0)
    message(FATAL_ERROR "weak-broken graph failed checking: ${weak_error}")
endif()
if(weak_error MATCHES "strong-reference-cycle")
    message(FATAL_ERROR "weak-broken graph emitted a cycle warning: ${weak_error}")
endif()

file(REMOVE "${cycle_source}" "${weak_source}")
