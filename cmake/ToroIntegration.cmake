include_guard(GLOBAL)

include(CMakeParseArguments)

function(toro_add_target)
    set(options)
    set(one_value_arguments NAME PROJECT)
    set(multi_value_arguments)
    cmake_parse_arguments(
        TORO_TARGET
        "${options}"
        "${one_value_arguments}"
        "${multi_value_arguments}"
        ${ARGN}
    )

    if(TORO_TARGET_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "toro_add_target received unknown arguments: ${TORO_TARGET_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TORO_TARGET_NAME)
        message(FATAL_ERROR "toro_add_target requires NAME")
    endif()
    if(NOT TORO_TARGET_PROJECT)
        message(FATAL_ERROR "toro_add_target requires PROJECT")
    endif()
    if(TARGET "${TORO_TARGET_NAME}")
        message(FATAL_ERROR "target '${TORO_TARGET_NAME}' already exists")
    endif()
    if(NOT CMAKE_C_COMPILER_LOADED)
        message(FATAL_ERROR
            "toro_add_target requires C to be enabled in the consuming project")
    endif()

    if(NOT TORO_EXECUTABLE)
        find_program(TORO_EXECUTABLE NAMES toro)
    endif()
    if(NOT TORO_EXECUTABLE)
        message(FATAL_ERROR
            "could not find the toro executable; set TORO_EXECUTABLE or add toro to PATH")
    endif()
    get_filename_component(TORO_EXECUTABLE "${TORO_EXECUTABLE}" ABSOLUTE)
    if(NOT EXISTS "${TORO_EXECUTABLE}")
        message(FATAL_ERROR "TORO_EXECUTABLE does not exist: ${TORO_EXECUTABLE}")
    endif()

    get_filename_component(
        toro_project_root "${TORO_TARGET_PROJECT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${toro_project_root}/toro.toml")
        message(FATAL_ERROR
            "Toro project manifest not found: ${toro_project_root}/toro.toml")
    endif()

    execute_process(
        COMMAND "${TORO_EXECUTABLE}" project-info --cmake
        WORKING_DIRECTORY "${toro_project_root}"
        RESULT_VARIABLE toro_info_status
        OUTPUT_VARIABLE toro_info
        ERROR_VARIABLE toro_info_error
    )
    if(NOT toro_info_status EQUAL 0)
        string(STRIP "${toro_info_error}" toro_info_error)
        message(FATAL_ERROR
            "could not inspect Toro project '${toro_project_root}': ${toro_info_error}")
    endif()

    set(toro_info_file
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TORO_TARGET_NAME}-toro-project.cmake")
    file(WRITE "${toro_info_file}" "${toro_info}")
    include("${toro_info_file}")

    file(GLOB_RECURSE toro_sources CONFIGURE_DEPENDS
        "${TORO_PROJECT_INFO_SOURCE_ROOT}/*.toro")
    if(NOT toro_sources)
        message(FATAL_ERROR
            "Toro project '${toro_project_root}' contains no .toro source files")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${toro_project_root}/toro.toml")

    set(toro_generated_directory
        "${CMAKE_CURRENT_BINARY_DIR}/toro-generated/${TORO_TARGET_NAME}")
    set(toro_generated_c "${toro_generated_directory}/program.c")
    add_custom_command(
        OUTPUT "${toro_generated_c}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${toro_generated_directory}"
        COMMAND "${TORO_EXECUTABLE}" emit-c -o "${toro_generated_c}"
        WORKING_DIRECTORY "${toro_project_root}"
        DEPENDS
            "${TORO_EXECUTABLE}"
            "${toro_project_root}/toro.toml"
            ${toro_sources}
        COMMENT "Generating C for Toro target ${TORO_TARGET_NAME}"
        VERBATIM
    )

    add_library("${TORO_TARGET_NAME}" STATIC "${toro_generated_c}")
    set_target_properties("${TORO_TARGET_NAME}" PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
        TORO_PROJECT_ROOT "${toro_project_root}"
    )
    if(TORO_PROJECT_INFO_C_LIBRARIES)
        target_link_libraries(
            "${TORO_TARGET_NAME}" INTERFACE ${TORO_PROJECT_INFO_C_LIBRARIES})
    endif()
endfunction()
