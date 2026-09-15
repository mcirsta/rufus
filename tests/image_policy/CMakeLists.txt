# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RufusImagePolicyTests LANGUAGES C)
    include(CTest)
endif()

# Also included by the Linux test build. No Qt or Windows SDK is needed.
add_executable(test_image_policy
    test_image_policy.c
    ../../src/image_policy.c
    ../../src/cregex_compile.c
    ../../src/cregex_parse.c
    ../../src/cregex_vm.c
)
target_include_directories(test_image_policy PRIVATE ../../src)
target_compile_features(test_image_policy PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_image_policy PRIVATE /W4)
    target_compile_definitions(test_image_policy PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_image_policy PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rufus_image_policy COMMAND test_image_policy)
set_tests_properties(rufus_image_policy PROPERTIES
    LABELS "component;image;no-hardware"
    TIMEOUT 30
)
