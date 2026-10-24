# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone qualification of the same native installer built by the application.
cmake_minimum_required(VERSION 3.21)
project(RufusSyslinuxProbe LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)
set(SYSLINUX_SOURCE_DIR "" CACHE PATH "Extracted upstream syslinux-6.04-pre1 release")
set(RUFUS_SYSLINUX_UPSTREAM "${SYSLINUX_SOURCE_DIR}")
get_filename_component(RUFUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_subdirectory(../../syslinux syslinux)

if(BUILD_TESTING)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    # The provider now comes from the shared native build; only the observer is
    # test-only. No new filesystem eligibility follows from building the tool.
    add_executable(syslinux_extlinux_facts extlinux_facts.c
        "${SYSLINUX_SOURCE_DIR}/libinstaller/syslxcom.c"
        "${SYSLINUX_SOURCE_DIR}/libinstaller/syslxrw.c")
    target_compile_features(syslinux_extlinux_facts PRIVATE c_std_11)
    target_compile_definitions(syslinux_extlinux_facts PRIVATE _FILE_OFFSET_BITS=64)
    target_compile_options(syslinux_extlinux_facts PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(syslinux_extlinux_facts PRIVATE "${SYSLINUX_SOURCE_DIR}/libinstaller")
    find_package(Qt6 6.2 REQUIRED COMPONENTS Core)
    add_executable(syslinux_command_probe command.c ../../platform/format_command.c)
    target_compile_features(syslinux_command_probe PRIVATE c_std_11)
    target_compile_definitions(syslinux_command_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(syslinux_command_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(syslinux_command_probe PRIVATE "${RUFUS_ROOT}/linux")
    target_link_options(syslinux_command_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=clock_gettime)
    add_test(NAME rufus_syslinux_native_command COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_command.py" "$<TARGET_FILE:syslinux_command_probe>"
        "$<TARGET_FILE:rufus-syslinux>" "${SYSLINUX_SOURCE_DIR}")
    set_tests_properties(rufus_syslinux_native_command PROPERTIES
        LABELS "functional;image;no-hardware" TIMEOUT 90)
    add_executable(syslinux_selection_probe selection.cpp ../../platform/syslinux_native.cpp
        "${RUFUS_ROOT}/src/syslinux_scan.c" "${RUFUS_ROOT}/src/syslinux_version.c"
        "${RUFUS_ROOT}/src/image_file.c")
    target_compile_features(syslinux_selection_probe PRIVATE cxx_std_17 c_std_11)
    target_compile_options(syslinux_selection_probe PRIVATE -Wall -Wextra -Wpedantic
        -include "${RUFUS_ROOT}/linux/platform/crt_strings.h")
    target_include_directories(syslinux_selection_probe PRIVATE
        "${RUFUS_ROOT}/linux" "${RUFUS_ROOT}/src")
    target_link_libraries(syslinux_selection_probe PRIVATE Qt6::Core)
    add_test(NAME rufus_syslinux_native_selection COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_selection.py" "$<TARGET_FILE:syslinux_selection_probe>"
        "${SYSLINUX_SOURCE_DIR}")
    set_tests_properties(rufus_syslinux_native_selection PROPERTIES
        LABELS "component;image;no-hardware" TIMEOUT 30)
    set(RUFUS_LINUX_BUILD_DIR "" CACHE PATH "Existing main Linux build for ISO extraction/scan probes")
    if(RUFUS_LINUX_BUILD_DIR)
        foreach(probe iso_extract_probe syslinux_scan_probe syslinux_source_probe syslinux_install_probe
                mbr_image_probe fat_format_probe)
            if(NOT EXISTS "${RUFUS_LINUX_BUILD_DIR}/${probe}")
                message(FATAL_ERROR "Build ${probe} in RUFUS_LINUX_BUILD_DIR before this qualification")
            endif()
        endforeach()
        find_program(SYSLINUX_MKISOFS NAMES mkisofs genisoimage REQUIRED)
        add_test(NAME rufus_syslinux_native_install COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_install.py" "${RUFUS_LINUX_BUILD_DIR}/syslinux_install_probe"
            "$<TARGET_FILE:rufus-syslinux>" "${SYSLINUX_SOURCE_DIR}" "${RUFUS_LINUX_BUILD_DIR}")
        set_tests_properties(rufus_syslinux_native_install PROPERTIES
            LABELS "functional;image;no-hardware" TIMEOUT 120)
        add_test(NAME rufus_syslinux_native_source COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_source.py" "${RUFUS_LINUX_BUILD_DIR}/syslinux_source_probe"
            "${SYSLINUX_SOURCE_DIR}" --mkisofs "${SYSLINUX_MKISOFS}")
        set_tests_properties(rufus_syslinux_native_source PROPERTIES
            LABELS "component;image;no-hardware" TIMEOUT 90)
        add_test(NAME rufus_syslinux_native_iso_layout COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_layout.py" "$<TARGET_FILE:rufus-syslinux>"
            "$<TARGET_FILE:syslinux_selection_probe>" "${SYSLINUX_SOURCE_DIR}" "${RUFUS_LINUX_BUILD_DIR}")
        set_tests_properties(rufus_syslinux_native_iso_layout PROPERTIES
            LABELS "functional;image;boot;no-hardware" TIMEOUT 180)
        add_test(NAME rufus_syslinux_native_mbr COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_layout.py" "$<TARGET_FILE:rufus-syslinux>"
            "$<TARGET_FILE:syslinux_selection_probe>" "${SYSLINUX_SOURCE_DIR}" "${RUFUS_LINUX_BUILD_DIR}" --mbr)
        set_tests_properties(rufus_syslinux_native_mbr PROPERTIES
            LABELS "functional;image;boot;no-hardware" TIMEOUT 90)
    endif()
    add_test(NAME rufus_syslinux_native_fat COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify.py" "$<TARGET_FILE:rufus-syslinux>"
        "${SYSLINUX_SOURCE_DIR}")
    set_tests_properties(rufus_syslinux_native_fat PROPERTIES
        LABELS "functional;boot;no-hardware" TIMEOUT 120)
endif()
