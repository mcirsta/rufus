# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RufusEfiBootInfoTests LANGUAGES C)
    include(CTest)
endif()

add_executable(test_efi_boot_info test_efi_boot_info.c ../../src/efi_boot_info.c)
target_include_directories(test_efi_boot_info PRIVATE ../../src)
target_compile_features(test_efi_boot_info PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_efi_boot_info PRIVATE /W4)
    target_compile_definitions(test_efi_boot_info PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_efi_boot_info PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rufus_efi_boot_info COMMAND test_efi_boot_info)
set_tests_properties(rufus_efi_boot_info PROPERTIES
    LABELS "component;image;no-hardware"
    TIMEOUT 30
)
