# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RufusSyslinuxVersionTests LANGUAGES C)
    include(CTest)
endif()

add_executable(test_syslinux_version test_syslinux_version.c ../../src/syslinux_version.c)
target_include_directories(test_syslinux_version PRIVATE ../../src)
target_compile_features(test_syslinux_version PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_syslinux_version PRIVATE /W4)
    target_compile_definitions(test_syslinux_version PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_syslinux_version PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rufus_syslinux_version COMMAND test_syslinux_version
    "${CMAKE_CURRENT_SOURCE_DIR}/../../res/syslinux/ldlinux_v4.sys"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../res/syslinux/ldlinux_v6.sys")
set_tests_properties(rufus_syslinux_version PROPERTIES
    LABELS "component;image;no-hardware"
    TIMEOUT 30
)
