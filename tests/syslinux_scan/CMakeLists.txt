# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RufusSyslinuxScanTests LANGUAGES C)
    include(CTest)
endif()

add_executable(test_syslinux_scan test_syslinux_scan.c ../../src/syslinux_scan.c)
target_include_directories(test_syslinux_scan PRIVATE ../../src)
target_compile_features(test_syslinux_scan PRIVATE c_std_11)
if(NOT WIN32)
    target_compile_options(test_syslinux_scan PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/../../linux/platform/crt_strings.h")
endif()
if(MSVC)
    target_compile_options(test_syslinux_scan PRIVATE /W4)
    target_compile_definitions(test_syslinux_scan PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_syslinux_scan PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rufus_syslinux_scan COMMAND test_syslinux_scan)
set_tests_properties(rufus_syslinux_scan PROPERTIES
    LABELS "component;image;no-hardware"
    TIMEOUT 30
)
