# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RufusImageFileTests LANGUAGES C)
    include(CTest)
endif()

add_executable(test_image_file test_image_file.c ../../src/image_file.c)
target_include_directories(test_image_file PRIVATE ../../src)
target_compile_features(test_image_file PRIVATE c_std_11)
if(NOT WIN32)
    target_compile_options(test_image_file PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/../../linux/platform/crt_strings.h")
endif()
if(MSVC)
    target_compile_options(test_image_file PRIVATE /W4)
    target_compile_definitions(test_image_file PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_image_file PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rufus_image_file COMMAND test_image_file)
set_tests_properties(rufus_image_file PROPERTIES
    LABELS "component;image;no-hardware"
    TIMEOUT 30
)
