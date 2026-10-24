# SPDX-License-Identifier: GPL-3.0-or-later
# File-only reuse audit. Nothing here is linked into or installed by the application.
cmake_minimum_required(VERSION 3.21)
project(RufusGrub2Probe LANGUAGES C)
include(CTest)
get_filename_component(RUFUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(grub_record_probe record.c
    "${RUFUS_ROOT}/src/ms-sys/br.c" "${RUFUS_ROOT}/src/image_file.c")
target_compile_features(grub_record_probe PRIVATE c_std_11)
target_compile_definitions(grub_record_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(grub_record_probe PRIVATE -Wall -Wextra -Wpedantic
    -include "${RUFUS_ROOT}/linux/platform/crt_strings.h")
target_include_directories(grub_record_probe PRIVATE "${RUFUS_ROOT}/src/ms-sys/inc")

set(RUFUS_LINUX_BUILD_DIR "" CACHE PATH "Existing Linux build with MBR and FAT probes")
set(RUFUS_GRUB_EVIDENCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/evidence" CACHE PATH "Compact qualification logs")
if(BUILD_TESTING AND RUFUS_LINUX_BUILD_DIR)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    foreach(program mbr_image_probe fat_format_probe)
        if(NOT EXISTS "${RUFUS_LINUX_BUILD_DIR}/${program}")
            message(FATAL_ERROR "Build ${program} in RUFUS_LINUX_BUILD_DIR first")
        endif()
    endforeach()
    find_program(GRUB_PROBE_QEMU qemu-system-x86_64 REQUIRED)
    find_program(GRUB_PROBE_MKFS mkfs.fat REQUIRED)
    find_program(GRUB_PROBE_MCOPY mcopy REQUIRED)
    add_test(NAME rufus_grub2_embedded_boot COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify.py" "$<TARGET_FILE:grub_record_probe>"
        "${RUFUS_LINUX_BUILD_DIR}" "${RUFUS_GRUB_EVIDENCE_DIR}"
        --qemu "${GRUB_PROBE_QEMU}" --mkfs "${GRUB_PROBE_MKFS}" --mcopy "${GRUB_PROBE_MCOPY}")
    set_tests_properties(rufus_grub2_embedded_boot PROPERTIES
        LABELS "functional;boot;image;no-hardware" TIMEOUT 90)
endif()
