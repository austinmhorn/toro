cmake_minimum_required(VERSION 3.25)

project(
    toro
    VERSION 0.1.0
    DESCRIPTION "The toro programming language"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(toro_core
    src/CGenerator.cpp
    src/Lexer.cpp
    src/NativeCompiler.cpp
    src/Parser.cpp
    src/SemanticAnalyzer.cpp
    src/SourceFile.cpp
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
endif()
