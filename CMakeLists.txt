cmake_minimum_required(VERSION 3.25)

project(
    Toro
    VERSION 0.1.0
    DESCRIPTION "The Toro programming language"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(toro
    src/main.cpp
)

target_include_directories(toro
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(toro PRIVATE /W4 /permissive-)
else()
    target_compile_options(toro PRIVATE -Wall -Wextra -Wpedantic)
endif()