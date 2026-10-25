# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone CP3 architecture experiment, not part of the application build.
cmake_minimum_required(VERSION 3.21)
project(RufusCdioProbe LANGUAGES C)
include("${CMAKE_CURRENT_LIST_DIR}/../../CompilerWarnings.cmake")
include(CTest)

get_filename_component(RUFUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_subdirectory(../../cdio cdio)

add_executable(probe_bundled probe.c)
# Inherits DO_NOT_WANT_COMPATIBILITY from bundled_cdio; probe.c uses that
# existing ABI definition for the bundled reader's 64-bit total_size field.
target_link_libraries(probe_bundled PRIVATE bundled_cdio)
target_compile_definitions(probe_bundled PRIVATE _FILE_OFFSET_BITS=64)
target_compile_options(probe_bundled PRIVATE -Wall -Wextra)

add_executable(probe_classify classify.c "${RUFUS_ROOT}/src/image_file.c")
target_link_libraries(probe_classify PRIVATE bundled_cdio)
target_include_directories(probe_classify PRIVATE "${RUFUS_ROOT}/src")
target_compile_options(probe_classify PRIVATE -Wall -Wextra
    -include "${RUFUS_ROOT}/linux/platform/crt_strings.h")
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(RUFUS_BSDTAR bsdtar REQUIRED)
    add_test(NAME rufus_iso_file_fixture COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_classification.py"
        "$<TARGET_FILE:probe_classify>" --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_iso_file_fixture PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
endif()

# Compile the same probe against the system library's own headers and ABI,
# without the bundled DO_NOT_WANT_COMPATIBILITY definition or its fields.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEM_CDIO REQUIRED IMPORTED_TARGET libiso9660 libudf)
add_executable(probe_system probe.c)
target_link_libraries(probe_system PRIVATE PkgConfig::SYSTEM_CDIO)
target_compile_definitions(probe_system PRIVATE _FILE_OFFSET_BITS=64)
target_compile_options(probe_system PRIVATE -Wall -Wextra)
