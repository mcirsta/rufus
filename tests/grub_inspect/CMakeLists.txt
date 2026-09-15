# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RufusGrubInspectionTests LANGUAGES C)
    include(CTest)
endif()

add_executable(test_grub_inspect test_grub_inspect.c
    ../../src/grub_inspect.c ../../src/strarray.c)
target_include_directories(test_grub_inspect PRIVATE ../../src)
target_compile_features(test_grub_inspect PRIVATE c_std_11)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_grub_inspect PRIVATE RUFUS_TEST_ALLOCATION_FAILURE)
    target_link_options(test_grub_inspect PRIVATE -Wl,--wrap=strdup)
endif()
if(NOT WIN32)
    target_compile_definitions(test_grub_inspect PRIVATE _GNU_SOURCE)
    target_compile_options(test_grub_inspect PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/../../linux/platform/crt_strings.h")
endif()
if(MSVC)
    target_compile_options(test_grub_inspect PRIVATE /W4)
    target_compile_definitions(test_grub_inspect PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_grub_inspect PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rufus_grub_inspect COMMAND test_grub_inspect)
set_tests_properties(rufus_grub_inspect PROPERTIES
    LABELS "component;image;no-hardware"
    TIMEOUT 30
)
