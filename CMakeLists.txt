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
    src/Lexer.cpp
    src/Parser.cpp
    src/SourceFile.cpp
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
endif()
