cmake_minimum_required(VERSION 3.25)

project(
    toro
    VERSION 0.1.0
    DESCRIPTION "The Toro programming language"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(toro_core
    src/ArcOptimizer.cpp
    src/BorrowInferenceAnalyzer.cpp
    src/CBackendModel.cpp
    src/CGenerator.cpp
    src/CGeneratorFrontend.cpp
    src/Diagnostic.cpp
    src/EffectAnalyzer.cpp
    src/EscapeAnalyzer.cpp
    src/Formatter.cpp
    src/IR.cpp
    src/IRLowerer.cpp
    src/Lexer.cpp
    src/ModuleLoader.cpp
    src/NativeCompiler.cpp
    src/OwnershipSummaryAnalyzer.cpp
    src/PerformanceContractAnalyzer.cpp
    src/Parser.cpp
    src/ProjectConfig.cpp
    src/SemanticAnalyzer.cpp
    src/SourceFile.cpp
    src/StandardLibrary.cpp
    src/StorageDecisionAnalyzer.cpp
    src/StrongCycleAnalyzer.cpp
    src/TestRunner.cpp
    src/TypeChecker.cpp
)

target_include_directories(toro_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_executable(toro src/main.cpp)
target_link_libraries(toro PRIVATE toro_core)

function(toro_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

toro_enable_warnings(toro_core)
toro_enable_warnings(toro)

if(BUILD_TESTING)
    add_executable(toro_lexer_tests tests/LexerTests.cpp)
    target_link_libraries(toro_lexer_tests PRIVATE toro_core)
    toro_enable_warnings(toro_lexer_tests)
    add_test(NAME lexer COMMAND toro_lexer_tests)

    add_executable(toro_parser_tests tests/ParserTests.cpp)
    target_link_libraries(toro_parser_tests PRIVATE toro_core)
    toro_enable_warnings(toro_parser_tests)
    add_test(NAME parser COMMAND toro_parser_tests)

    add_executable(toro_semantic_tests tests/SemanticAnalyzerTests.cpp)
    target_link_libraries(toro_semantic_tests PRIVATE toro_core)
    toro_enable_warnings(toro_semantic_tests)
    add_test(NAME semantic COMMAND toro_semantic_tests)

    add_executable(toro_type_checker_tests tests/TypeCheckerTests.cpp)
    target_link_libraries(toro_type_checker_tests PRIVATE toro_core)
    toro_enable_warnings(toro_type_checker_tests)
    add_test(NAME type_checker COMMAND toro_type_checker_tests)

    add_executable(toro_ir_tests tests/IRTests.cpp)
    target_link_libraries(toro_ir_tests PRIVATE toro_core)
    target_compile_definitions(toro_ir_tests PRIVATE
        TORO_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    toro_enable_warnings(toro_ir_tests)
    add_test(NAME ir COMMAND toro_ir_tests)

    add_executable(toro_effect_analyzer_tests tests/EffectAnalyzerTests.cpp)
    target_link_libraries(toro_effect_analyzer_tests PRIVATE toro_core)
    target_compile_definitions(toro_effect_analyzer_tests PRIVATE
        TORO_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    toro_enable_warnings(toro_effect_analyzer_tests)
    add_test(NAME effect_analyzer COMMAND toro_effect_analyzer_tests)

    add_executable(toro_performance_contract_tests
        tests/PerformanceContractAnalyzerTests.cpp)
    target_link_libraries(toro_performance_contract_tests PRIVATE toro_core)
    toro_enable_warnings(toro_performance_contract_tests)
    add_test(NAME performance_contract COMMAND toro_performance_contract_tests)
    add_test(NAME performance_contract_cli
        COMMAND ${CMAKE_COMMAND}
            -DTORO_EXECUTABLE=$<TARGET_FILE:toro>
            -DTORO_BINARY_DIR=${PROJECT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/PerformanceContractCliTest.cmake)

    add_executable(toro_escape_analyzer_tests tests/EscapeAnalyzerTests.cpp)
    target_link_libraries(toro_escape_analyzer_tests PRIVATE toro_core)
    toro_enable_warnings(toro_escape_analyzer_tests)
    add_test(NAME escape_analyzer COMMAND toro_escape_analyzer_tests)

    add_executable(toro_storage_decision_tests tests/StorageDecisionTests.cpp)
    target_link_libraries(toro_storage_decision_tests PRIVATE toro_core)
    toro_enable_warnings(toro_storage_decision_tests)
    add_test(NAME storage_decision COMMAND toro_storage_decision_tests)

    add_executable(toro_borrow_inference_tests tests/BorrowInferenceTests.cpp)
    target_link_libraries(toro_borrow_inference_tests PRIVATE toro_core)
    toro_enable_warnings(toro_borrow_inference_tests)
    add_test(NAME borrow_inference COMMAND toro_borrow_inference_tests)

    add_executable(toro_ownership_summary_tests tests/OwnershipSummaryTests.cpp)
    target_link_libraries(toro_ownership_summary_tests PRIVATE toro_core)
    toro_enable_warnings(toro_ownership_summary_tests)
    add_test(NAME ownership_summary COMMAND toro_ownership_summary_tests)

    add_executable(toro_strong_cycle_tests tests/StrongCycleAnalyzerTests.cpp)
    target_link_libraries(toro_strong_cycle_tests PRIVATE toro_core)
    toro_enable_warnings(toro_strong_cycle_tests)
    add_test(NAME strong_cycle COMMAND toro_strong_cycle_tests)
    add_test(NAME strong_cycle_cli
        COMMAND ${CMAKE_COMMAND}
            -DTORO_EXECUTABLE=$<TARGET_FILE:toro>
            -DTORO_BINARY_DIR=${PROJECT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/StrongCycleCliTest.cmake)

    add_executable(toro_arc_optimizer_tests tests/ArcOptimizerTests.cpp)
    target_link_libraries(toro_arc_optimizer_tests PRIVATE toro_core)
    target_compile_definitions(toro_arc_optimizer_tests PRIVATE
        TORO_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    toro_enable_warnings(toro_arc_optimizer_tests)
    add_test(NAME arc_optimizer COMMAND toro_arc_optimizer_tests)

    add_executable(toro_c_generator_tests tests/CGeneratorTests.cpp)
    target_link_libraries(toro_c_generator_tests PRIVATE toro_core)
    toro_enable_warnings(toro_c_generator_tests)
    find_program(TORO_TEST_C_COMPILER NAMES cc clang gcc)
    if(TORO_TEST_C_COMPILER)
        target_compile_definitions(toro_c_generator_tests PRIVATE
            TORO_TEST_C_COMPILER="${TORO_TEST_C_COMPILER}"
        )
    endif()
    add_test(NAME c_generator COMMAND toro_c_generator_tests)

    add_executable(toro_native_compiler_tests tests/NativeCompilerTests.cpp)
    target_link_libraries(toro_native_compiler_tests PRIVATE toro_core)
    toro_enable_warnings(toro_native_compiler_tests)
    add_test(NAME native_compiler COMMAND toro_native_compiler_tests)

    add_executable(toro_module_tests tests/ModuleLoaderTests.cpp)
    target_link_libraries(toro_module_tests PRIVATE toro_core)
    toro_enable_warnings(toro_module_tests)
    add_test(NAME modules COMMAND toro_module_tests)

    add_executable(toro_project_config_tests tests/ProjectConfigTests.cpp)
    target_link_libraries(toro_project_config_tests PRIVATE toro_core)
    toro_enable_warnings(toro_project_config_tests)
    add_test(NAME project_config COMMAND toro_project_config_tests)

    add_executable(toro_standard_library_tests tests/StandardLibraryTests.cpp)
    target_link_libraries(toro_standard_library_tests PRIVATE toro_core)
    toro_enable_warnings(toro_standard_library_tests)
    add_test(NAME standard_library COMMAND toro_standard_library_tests)

    add_executable(toro_formatter_tests tests/FormatterTests.cpp)
    target_link_libraries(toro_formatter_tests PRIVATE toro_core)
    toro_enable_warnings(toro_formatter_tests)
    add_test(NAME formatter COMMAND toro_formatter_tests)

    add_executable(toro_test_runner_tests tests/TestRunnerTests.cpp)
    target_link_libraries(toro_test_runner_tests PRIVATE toro_core)
    toro_enable_warnings(toro_test_runner_tests)
    add_test(NAME test_runner COMMAND toro_test_runner_tests)

    add_executable(toro_ffi_tests tests/FFITests.cpp)
    target_link_libraries(toro_ffi_tests PRIVATE toro_core)
    toro_enable_warnings(toro_ffi_tests)
    add_test(NAME ffi COMMAND toro_ffi_tests)

    add_test(NAME project_cli_check COMMAND toro check)
    set_tests_properties(project_cli_check PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_ir COMMAND toro ir)
    set_tests_properties(project_cli_ir PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_ir_effects COMMAND toro ir --effects)
    set_tests_properties(project_cli_ir_effects PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_ir_ownership COMMAND toro ir --ownership)
    set_tests_properties(project_cli_ir_ownership PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_ir_escape COMMAND toro ir --escape)
    set_tests_properties(project_cli_ir_escape PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_ir_arc COMMAND toro ir --arc)
    set_tests_properties(project_cli_ir_arc PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_emit_c COMMAND toro emit-c)
    set_tests_properties(project_cli_emit_c PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_build
        COMMAND toro build -o ${PROJECT_BINARY_DIR}/toro-project-example-test)
    set_tests_properties(project_cli_build PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)
    add_test(NAME project_cli_run COMMAND toro run)
    set_tests_properties(project_cli_run PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/project)

    add_test(NAME stdlib_cli_check COMMAND toro check)
    set_tests_properties(stdlib_cli_check PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/stdlib)
    add_test(NAME stdlib_cli_run COMMAND toro run)
    set_tests_properties(stdlib_cli_run PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/stdlib)

    add_test(NAME testing_cli_fmt_check COMMAND toro fmt --check)
    set_tests_properties(testing_cli_fmt_check PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/testing)
    add_test(NAME testing_cli_test COMMAND toro test)
    set_tests_properties(testing_cli_test PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/testing)

    add_test(NAME ffi_cli_check COMMAND toro check)
    set_tests_properties(ffi_cli_check PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/ffi)
    add_test(NAME ffi_cli_run COMMAND toro run)
    set_tests_properties(ffi_cli_run PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/ffi)
    add_test(NAME ffi_cli_build
        COMMAND toro build -o ${PROJECT_BINARY_DIR}/toro-ffi-example-test)
    set_tests_properties(ffi_cli_build PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/ffi)

    add_test(NAME cmake_integration
        COMMAND ${CMAKE_COMMAND}
            -DTORO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DTORO_BINARY_DIR=${PROJECT_BINARY_DIR}
            -DTORO_EXECUTABLE=$<TARGET_FILE:toro>
            -P ${PROJECT_SOURCE_DIR}/tests/CMakeIntegrationTest.cmake)
endif()
