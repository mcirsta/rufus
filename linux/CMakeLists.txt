# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(RufusLinux LANGUAGES C CXX)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core)
option(RUFUS_BUILD_GUI "Build the Qt Widgets application" ON)
option(RUFUS_BUILD_VM_TEST "Build the guarded, guest-only native USB test" OFF)
option(RUFUS_COMPARE_SYSTEM_WIMLIB "Compare the extended upstream reader with a system wimlib in tests" OFF)
include(CTest)
if(RUFUS_BUILD_VM_TEST AND NOT BUILD_TESTING)
    message(FATAL_ERROR "RUFUS_BUILD_VM_TEST requires BUILD_TESTING=ON")
endif()

add_library(rufus_qt_reporting STATIC
    qt/rufus_job.cpp
    qt/rufus_job.h
    platform/rufus_reporting.h
)
set_target_properties(rufus_qt_reporting PROPERTIES AUTOMOC ON)
target_compile_features(rufus_qt_reporting PUBLIC cxx_std_17)
target_include_directories(rufus_qt_reporting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rufus_qt_reporting PUBLIC Qt6::Core)
target_compile_options(rufus_qt_reporting PRIVATE -Wall -Wextra -Wpedantic)

if(RUFUS_BUILD_GUI OR BUILD_TESTING)
    add_subdirectory(cdio)
    add_library(bundled_fat STATIC
        ../src/syslinux/libfat/open.c
        ../src/syslinux/libfat/cache.c
        ../src/syslinux/libfat/searchdir.c
        ../src/syslinux/libfat/fatchain.c
    )
    target_compile_features(bundled_fat PRIVATE c_std_11)
    target_compile_definitions(bundled_fat PRIVATE _GNU_SOURCE)
    target_compile_options(bundled_fat PRIVATE -Wall -Wextra
        -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/fat_alloc.h")
    add_library(rufus_iso_scan STATIC
        ../src/iso_scan.c
        ../src/iso_read.c
        ../src/iso_analyze.c
        ../src/iso_bootloaders.c
        ../src/embedded_efi.c
        ../src/iso_file.c
        ../src/winpe_scan.c
        ../src/image_file.c
        ../src/image_policy.c
        ../src/cregex_compile.c
        ../src/cregex_parse.c
        ../src/cregex_vm.c
        ../src/strarray.c
        ../src/syslinux_version.c
        ../src/syslinux_scan.c
        ../src/grub_inspect.c
        ../src/efi_boot_info.c
        ../src/pe_arch.c
    )
    target_compile_features(rufus_iso_scan PRIVATE c_std_11)
    target_compile_definitions(rufus_iso_scan PRIVATE _GNU_SOURCE)
    target_compile_options(rufus_iso_scan PRIVATE -Wall -Wextra -Wpedantic
        -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/crt_strings.h")
    target_include_directories(rufus_iso_scan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_iso_scan PUBLIC bundled_cdio bundled_fat)
    add_library(rufus_image_source STATIC platform/image_source.c)
    target_compile_definitions(rufus_image_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_image_source PRIVATE -Wall -Wextra)
    target_include_directories(rufus_image_source PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_image_source PUBLIC rufus_iso_scan)
    add_library(rufus_raw_write STATIC platform/raw_write.c)
    target_compile_definitions(rufus_raw_write PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_raw_write PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_raw_write PUBLIC rufus_image_source)
    add_library(rufus_iso_extract STATIC platform/iso_extract.c platform/iso_extract.h)
    target_compile_definitions(rufus_iso_extract PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_iso_extract PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_iso_extract PUBLIC rufus_image_source)
    add_library(rufus_fat_format STATIC platform/fat_format.c platform/fat_format.h)
    target_compile_features(rufus_fat_format PRIVATE c_std_11)
    target_compile_definitions(rufus_fat_format PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_fat_format PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_fat_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_fat_format PUBLIC bundled_fat)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RUFUS_FDISK REQUIRED IMPORTED_TARGET fdisk>=2.35)
    add_library(rufus_partition_layout STATIC ../src/partition_layout.c)
    target_include_directories(rufus_partition_layout PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_features(rufus_partition_layout PRIVATE c_std_11)
    target_compile_options(rufus_partition_layout PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_partition_image STATIC platform/partition_image.c platform/partition_image.h)
    target_compile_features(rufus_partition_image PRIVATE c_std_11)
    target_compile_definitions(rufus_partition_image PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_include_directories(rufus_partition_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(rufus_partition_image PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_partition_image PUBLIC rufus_partition_layout PkgConfig::RUFUS_FDISK)
    find_program(RUFUS_MKFS_FAT mkfs.fat REQUIRED)
    add_library(rufus_prepare_process STATIC qt/prepare_image_protocol.cpp qt/prepare_image_protocol.h
        qt/prepare_image_process.cpp qt/prepare_image_process.h)
    set_target_properties(rufus_prepare_process PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_prepare_process PUBLIC cxx_std_17)
    target_include_directories(rufus_prepare_process PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_prepare_process PUBLIC Qt6::Core)
    target_compile_options(rufus_prepare_process PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(rufus-prepare-helper helper/prepare_main.cpp)
    set_target_properties(rufus-prepare-helper PROPERTIES AUTOMOC ON)
    target_compile_definitions(rufus-prepare-helper PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}")
    target_compile_options(rufus-prepare-helper PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_prepare_process rufus_qt_reporting
        rufus_partition_image rufus_fat_format)
    add_subdirectory(wimlib)
    add_library(rufus_wim_metadata STATIC ../src/wim_metadata.c)
    target_compile_features(rufus_wim_metadata PRIVATE c_std_11)
    target_compile_options(rufus_wim_metadata PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_wim_metadata PUBLIC rufus_wimlib)
    add_library(rufus_wim_file STATIC platform/wim_file.c platform/wim_file.h)
    target_compile_features(rufus_wim_file PRIVATE c_std_11)
    target_compile_definitions(rufus_wim_file PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_wim_file PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_wim_file PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_wim_file PUBLIC rufus_wim_metadata)
    add_library(rufus_wim_read_file STATIC platform/wim_read_file.c)
    target_compile_definitions(rufus_wim_read_file PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_wim_read_file PRIVATE -Wall -Wextra)
    target_include_directories(rufus_wim_read_file PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_wim_read_file PUBLIC rufus_wimlib)
    add_library(rufus_windows_image STATIC platform/windows_image.c platform/windows_image.h)
    target_compile_options(rufus_windows_image PRIVATE -Wall -Wextra)
    target_include_directories(rufus_windows_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_image PUBLIC rufus_wim_metadata)
    add_library(rufus_qt_analysis STATIC
        qt/image_analysis_job.cpp
        qt/image_analysis_job.h
        qt/image_resources.qrc
    )
    set_target_properties(rufus_qt_analysis PROPERTIES AUTOMOC ON AUTORCC ON)
    target_link_libraries(rufus_qt_analysis PUBLIC rufus_windows_image rufus_qt_reporting rufus_image_source)
    target_compile_options(rufus_qt_analysis PRIVATE -Wall -Wextra -Wpedantic)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets DBus)
    add_library(rufus_udisks STATIC
        platform/device.h
        platform/udisks_client.cpp
        platform/udisks_client.h
    )
    set_target_properties(rufus_udisks PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_udisks PUBLIC cxx_std_17)
    target_include_directories(rufus_udisks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_udisks PUBLIC Qt6::DBus)
    target_compile_options(rufus_udisks PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_prepare_device STATIC platform/prepare_device.cpp platform/prepare_device.h)
    target_compile_definitions(rufus_prepare_device PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_prepare_device PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_prepare_device PUBLIC rufus_udisks rufus_prepare_process rufus_fat_format)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_prepare_device)
    include(GNUInstallDirs)
    configure_file(helper/org.rufus.PrepareDevice.policy.in org.rufus.PrepareDevice.policy @ONLY)
    install(TARGETS rufus-prepare-helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.rufus.PrepareDevice.policy"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
    add_library(rufus_raw_target STATIC platform/raw_write_plan.cpp platform/raw_write_plan.h
        platform/udisks_target.cpp platform/udisks_target.h)
    set_target_properties(rufus_raw_target PROPERTIES AUTOMOC ON)
    target_compile_definitions(rufus_raw_target PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_raw_target PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_raw_target PUBLIC rufus_udisks rufus_raw_write)
    add_library(rufus_qt_write STATIC qt/raw_write_job.cpp qt/raw_write_job.h
        qt/raw_write_controller.cpp qt/raw_write_controller.h)
    set_target_properties(rufus_qt_write PROPERTIES AUTOMOC ON)
    target_compile_options(rufus_qt_write PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_qt_write PUBLIC rufus_raw_target rufus_qt_reporting)
    add_library(rufus_qt_widgets STATIC
        qt/main_window.cpp
        qt/main_window.h
        qt/main_window.ui
        qt/rufus_icons.qrc
        qt/selection_controller.cpp
        qt/selection_controller.h
        qt/image_mode_dialog.cpp
        qt/image_mode_dialog.h
    )
    target_link_libraries(rufus_qt_widgets PUBLIC rufus_qt_write)
    set_target_properties(rufus_qt_widgets PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    target_compile_features(rufus_qt_widgets PUBLIC cxx_std_17)
    target_include_directories(rufus_qt_widgets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_qt_widgets PUBLIC Qt6::Widgets rufus_qt_analysis)
    target_compile_options(rufus_qt_widgets PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(RUFUS_BUILD_GUI)
    add_executable(rufus-linux qt/main.cpp)
    target_link_libraries(rufus-linux PRIVATE rufus_qt_widgets rufus_udisks)
    target_compile_options(rufus-linux PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(raw_write_probe tests/raw_write_probe.c)
    target_compile_definitions(raw_write_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(raw_write_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(raw_write_probe PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64
        -Wl,--wrap=pwrite -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    target_link_libraries(raw_write_probe PRIVATE rufus_raw_write)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME rufus_raw_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_raw_write.py" "$<TARGET_FILE:raw_write_probe>")
    set_tests_properties(rufus_raw_write PROPERTIES LABELS "functional;write;no-hardware" TIMEOUT 30)
    add_executable(test_image_source tests/test_image_source.c)
    target_compile_definitions(test_image_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_image_source PRIVATE -Wall -Wextra)
    target_link_options(test_image_source PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64)
    target_link_libraries(test_image_source PRIVATE rufus_image_source)
    add_test(NAME rufus_image_source COMMAND test_image_source)
    set_tests_properties(rufus_image_source PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 10)
    add_executable(iso_file_probe tests/iso_file_probe.c)
    target_compile_options(iso_file_probe PRIVATE -Wall -Wextra)
    target_link_options(iso_file_probe PRIVATE -Wl,--wrap=iso9660_iso_seek_read -Wl,--wrap=udf_read_block)
    target_link_libraries(iso_file_probe PRIVATE rufus_iso_scan)
    add_executable(wim_fixture tests/wim_fixture.c)
    target_compile_options(wim_fixture PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(wim_fixture PRIVATE rufus_wim_metadata)
    include(wimlib/tests.cmake)
    add_executable(test_iso_file_contract tests/test_iso_file_contract.c)
    target_compile_options(test_iso_file_contract PRIVATE -Wall -Wextra)
    target_link_options(test_iso_file_contract PRIVATE
        -Wl,--wrap=iso9660_iso_seek_read -Wl,--wrap=udf_setpos -Wl,--wrap=udf_read_block)
    target_link_libraries(test_iso_file_contract PRIVATE rufus_iso_scan)
    add_test(NAME rufus_iso_file_contract COMMAND test_iso_file_contract)
    set_tests_properties(rufus_iso_file_contract PROPERTIES
        LABELS "component;image;no-hardware" TIMEOUT 10)
    add_executable(test_wim_metadata tests/test_wim_metadata.c)
    target_compile_options(test_wim_metadata PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_wim_metadata PRIVATE rufus_wim_file)
    add_test(NAME rufus_wim_metadata COMMAND test_wim_metadata)
    set_tests_properties(rufus_wim_metadata PROPERTIES
        LABELS "functional;component;image;no-hardware" TIMEOUT 30)
    add_executable(test_wim_contract tests/test_wim_contract.c)
    target_compile_options(test_wim_contract PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_wim_contract PRIVATE
        -Wl,--wrap=wimlib_open_wim -Wl,--wrap=wimlib_free
        -Wl,--wrap=wimlib_get_wim_info -Wl,--wrap=wimlib_get_image_property)
    target_link_libraries(test_wim_contract PRIVATE rufus_wim_file)
    add_test(NAME rufus_wim_contract COMMAND test_wim_contract)
    set_tests_properties(rufus_wim_contract PROPERTIES
        LABELS "component;image;no-hardware" TIMEOUT 10)
    add_subdirectory(../tests/image_policy image-policy)
    add_subdirectory(../tests/image_file image-file)
    add_subdirectory(../tests/partition_layout partition-layout)
    add_subdirectory(../tests/syslinux_version syslinux-version)
    add_subdirectory(../tests/syslinux_scan syslinux-scan)
    add_subdirectory(../tests/grub_inspect grub-inspect)
    add_subdirectory(../tests/efi_boot_info efi-boot-info)
    add_subdirectory(../tests/pe_arch pe-arch)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(RUFUS_BSDTAR bsdtar REQUIRED)
    find_program(RUFUS_MKISOFS NAMES genisoimage mkisofs REQUIRED)
    add_executable(iso_extract_probe tests/iso_extract_probe.c)
    target_compile_definitions(iso_extract_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(iso_extract_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(iso_extract_probe PRIVATE rufus_iso_extract)
    target_link_options(iso_extract_probe PRIVATE -Wl,--wrap=openat -Wl,--wrap=openat64
        -Wl,--wrap=ftruncate -Wl,--wrap=ftruncate64
        -Wl,--wrap=mkdirat -Wl,--wrap=write -Wl,--wrap=close -Wl,--wrap=fsync
        -Wl,--wrap=ReadIsoFileRange -Wl,--wrap=AnalyzeIsoImageWithSource)
    add_test(NAME rufus_iso_extract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_extract.py" "$<TARGET_FILE:iso_extract_probe>"
        --mkisofs "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_iso_extract PROPERTIES
        LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    find_program(RUFUS_MKFS_FAT mkfs.fat REQUIRED)
    find_program(RUFUS_FSCK_FAT fsck.fat REQUIRED)
    find_program(RUFUS_MDIR mdir REQUIRED)
    find_program(RUFUS_SFDISK sfdisk REQUIRED)
    add_executable(partition_image_probe tests/partition_image_probe.c)
    target_compile_definitions(partition_image_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(partition_image_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(partition_image_probe PRIVATE rufus_partition_image)
    target_link_options(partition_image_probe PRIVATE -Wl,--wrap=fsync
        -Wl,--wrap=fdisk_add_partition -Wl,--wrap=fdisk_write_disklabel)
    add_test(NAME rufus_partition_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_partition_image.py"
        "$<TARGET_FILE:partition_image_probe>" "$<TARGET_FILE:fat_format_probe>"
        --sfdisk "${RUFUS_SFDISK}" --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_partition_image PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_executable(fat_format_probe tests/fat_format_probe.c)
    target_compile_definitions(fat_format_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(fat_format_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fat_format_probe PRIVATE rufus_fat_format)
    target_link_options(fat_format_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=clock_gettime)
    add_test(NAME rufus_fat_format COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat_format.py" "$<TARGET_FILE:fat_format_probe>"
        --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_fat_format PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    find_program(RUFUS_MCOPY mcopy REQUIRED)
    find_program(RUFUS_MMD mmd REQUIRED)
    add_test(NAME rufus_iso_file_ranges COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_file.py"
        "$<TARGET_FILE:iso_file_probe>" "$<TARGET_FILE:wim_fixture>" --mkisofs "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_iso_file_ranges PROPERTIES
        LABELS "functional;image;no-hardware" TIMEOUT 60)
    add_executable(test_winpe_options tests/test_winpe_options.c)
    target_compile_options(test_winpe_options PRIVATE -Wall -Wextra)
    target_link_libraries(test_winpe_options PRIVATE rufus_iso_scan)
    add_test(NAME rufus_winpe_options COMMAND test_winpe_options)
    set_tests_properties(rufus_winpe_options PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 10)
    add_test(NAME rufus_winpe_images COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_winpe_images.py" "$<TARGET_FILE:iso_analyze_probe>"
        --bsdtar "${RUFUS_BSDTAR}" --mkisofs "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_winpe_images PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 60)
    add_executable(embedded_efi_probe tests/embedded_efi_probe.c)
    target_compile_options(embedded_efi_probe PRIVATE -Wall -Wextra)
    target_link_options(embedded_efi_probe PRIVATE
        -Wl,--wrap=iso9660_iso_seek_read -Wl,--wrap=calloc
        -Wl,--wrap=malloc -Wl,--wrap=posix_memalign
        -Wl,--wrap=udf_read_block -Wl,--wrap=udf_setpos
        -Wl,--wrap=udf_get_root -Wl,--wrap=udf_fopen)
    target_link_libraries(embedded_efi_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_embedded_efi COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_embedded_efi.py"
        "$<TARGET_FILE:embedded_efi_probe>" "$<TARGET_FILE:iso_analyze_probe>"
        --bsdtar "${RUFUS_BSDTAR}" --mkisofs "${RUFUS_MKISOFS}"
        --mkfs-fat "${RUFUS_MKFS_FAT}" --mcopy "${RUFUS_MCOPY}" --mmd "${RUFUS_MMD}")
    set_tests_properties(rufus_embedded_efi PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(udf_range_probe tests/udf_range_probe.c)
    target_compile_options(udf_range_probe PRIVATE -Wall -Wextra)
    target_link_options(udf_range_probe PRIVATE -Wl,--wrap=cdio_stream_read)
    target_link_libraries(udf_range_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_udf_embedded_efi COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_embedded_efi.py"
        "$<TARGET_FILE:embedded_efi_probe>" "$<TARGET_FILE:iso_analyze_probe>"
        "$<TARGET_FILE:udf_range_probe>" --mkisofs "${RUFUS_MKISOFS}"
        --mkfs-fat "${RUFUS_MKFS_FAT}" --mcopy "${RUFUS_MCOPY}" --mmd "${RUFUS_MMD}")
    set_tests_properties(rufus_udf_embedded_efi PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(udf_reader_probe tests/udf_reader_probe.c)
    target_compile_options(udf_reader_probe PRIVATE -Wall -Wextra)
    target_link_options(udf_reader_probe PRIVATE -Wl,--wrap=malloc -Wl,--wrap=cdio_stream_read)
    target_link_libraries(udf_reader_probe PRIVATE rufus_iso_scan)
    add_executable(iso_analyze_probe tests/iso_analyze_probe.c)
    target_compile_options(iso_analyze_probe PRIVATE -Wall -Wextra)
    target_link_options(iso_analyze_probe PRIVATE
        -Wl,--wrap=ReadISOFileToBufferExt -Wl,--wrap=ReadISOFileToBufferLimited -Wl,--wrap=StrArrayCreate -Wl,--wrap=strdup
        -Wl,--wrap=udf_readdir -Wl,--wrap=udf_opendir)
    target_link_libraries(iso_analyze_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_iso_analysis COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_analyze.py"
        "$<TARGET_FILE:iso_analyze_probe>" --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_iso_analysis PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_test(NAME rufus_udf_images COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_images.py"
        "$<TARGET_FILE:iso_analyze_probe>" "$<TARGET_FILE:iso_read_probe>"
        "$<TARGET_FILE:udf_reader_probe>"
        --mkisofs "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_udf_images PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(test_iso_analyze_contract tests/test_iso_analyze_contract.c)
    target_compile_options(test_iso_analyze_contract PRIVATE -Wall -Wextra)
    target_link_options(test_iso_analyze_contract PRIVATE
        -Wl,--wrap=udf_open -Wl,--wrap=udf_close -Wl,--wrap=udf_get_root
        -Wl,--wrap=iso9660_open_ext)
    target_link_libraries(test_iso_analyze_contract PRIVATE rufus_iso_scan)
    add_test(NAME rufus_iso_analysis_contract COMMAND test_iso_analyze_contract)
    set_tests_properties(rufus_iso_analysis_contract PROPERTIES
        LABELS "component;image;no-hardware"
        TIMEOUT 10
    )
    add_executable(syslinux_scan_probe tests/syslinux_scan_probe.c)
    target_compile_options(syslinux_scan_probe PRIVATE -Wall -Wextra)
    target_link_libraries(syslinux_scan_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_syslinux_scan_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_syslinux_scan.py"
        "$<TARGET_FILE:syslinux_scan_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/syslinux"
        --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_syslinux_scan_image PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(efi_boot_info_probe tests/efi_boot_info_probe.c)
    target_compile_options(efi_boot_info_probe PRIVATE -Wall -Wextra)
    target_link_libraries(efi_boot_info_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_efi_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_efi_image.py"
        "$<TARGET_FILE:efi_boot_info_probe>" --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_efi_image PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(grub_inspect_probe tests/grub_inspect_probe.c)
    target_compile_options(grub_inspect_probe PRIVATE -Wall -Wextra)
    target_link_libraries(grub_inspect_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_grub_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_grub_image.py"
        "$<TARGET_FILE:grub_inspect_probe>" --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_grub_image PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(syslinux_version_probe tests/syslinux_version_probe.c)
    target_compile_options(syslinux_version_probe PRIVATE -Wall -Wextra)
    target_link_libraries(syslinux_version_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_syslinux_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_syslinux_image.py"
        "$<TARGET_FILE:syslinux_version_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/syslinux"
        --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_syslinux_image PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(iso_read_probe tests/iso_read_probe.c)
    target_compile_options(iso_read_probe PRIVATE -Wall -Wextra)
    target_link_libraries(iso_read_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_iso_contents COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_read.py"
        "$<TARGET_FILE:iso_read_probe>" --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_iso_contents PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(test_iso_read_contract tests/test_iso_read_contract.c)
    target_compile_options(test_iso_read_contract PRIVATE -Wall -Wextra)
    target_link_options(test_iso_read_contract PRIVATE -Wl,--wrap=malloc)
    target_link_libraries(test_iso_read_contract PRIVATE rufus_iso_scan)
    add_test(NAME rufus_iso_read_contract COMMAND test_iso_read_contract)
    set_tests_properties(rufus_iso_read_contract PROPERTIES
        LABELS "component;image;no-hardware"
        TIMEOUT 10
    )
    add_executable(iso_scan_probe tests/iso_scan_probe.c)
    target_compile_definitions(iso_scan_probe PRIVATE _FILE_OFFSET_BITS=64)
    target_compile_options(iso_scan_probe PRIVATE -Wall -Wextra)
    target_link_libraries(iso_scan_probe PRIVATE rufus_iso_scan)
    add_test(NAME rufus_iso_tree COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_scan.py"
        "$<TARGET_FILE:iso_scan_probe>" --bsdtar "${RUFUS_BSDTAR}")
    set_tests_properties(rufus_iso_tree PROPERTIES
        LABELS "functional;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(test_strarray tests/test_strarray.c)
    target_compile_definitions(test_strarray PRIVATE _GNU_SOURCE)
    target_compile_options(test_strarray PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_strarray PRIVATE -Wl,--wrap=realloc)
    target_link_libraries(test_strarray PRIVATE rufus_iso_scan)
    add_test(NAME rufus_strarray COMMAND test_strarray)
    set_tests_properties(rufus_strarray PROPERTIES
        LABELS "component;image;no-hardware"
        TIMEOUT 10
    )
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
    add_executable(prepare_helper_probe helper/prepare_main.cpp tests/prepare_helper_faults.c)
    set_target_properties(prepare_helper_probe PROPERTIES AUTOMOC ON)
    target_compile_definitions(prepare_helper_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}")
    target_compile_options(prepare_helper_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(prepare_helper_probe PRIVATE -Wl,--wrap=posix_spawn -Wl,--wrap=fsync)
    target_link_libraries(prepare_helper_probe PRIVATE rufus_prepare_process rufus_qt_reporting
        rufus_partition_image rufus_fat_format rufus_prepare_device)
    add_executable(test_prepare_image_process tests/test_prepare_image_process.cpp)
    set_target_properties(test_prepare_image_process PROPERTIES AUTOMOC ON)
    target_compile_options(test_prepare_image_process PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_prepare_image_process PRIVATE
        RUFUS_PREPARE_HELPER="$<TARGET_FILE:rufus-prepare-helper>"
        RUFUS_PREPARE_PROBE="$<TARGET_FILE:prepare_helper_probe>")
    target_link_libraries(test_prepare_image_process PRIVATE rufus_prepare_process Qt6::Test)
    add_dependencies(test_prepare_image_process rufus-prepare-helper prepare_helper_probe)
    add_test(NAME rufus_prepare_process COMMAND test_prepare_image_process)
    set_tests_properties(rufus_prepare_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30)
    add_test(NAME rufus_prepare_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_helper.py" "$<TARGET_FILE:rufus-prepare-helper>"
        --probe "$<TARGET_FILE:prepare_helper_probe>" --sfdisk "${RUFUS_SFDISK}"
        --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_prepare_helper PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 60)
    if(RUFUS_BUILD_VM_TEST)
        add_executable(test_prepare_device_guest tests/vm/test_prepare_device_guest.cpp)
        set_target_properties(test_prepare_device_guest PROPERTIES AUTOMOC ON)
        target_compile_options(test_prepare_device_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_prepare_device_guest PRIVATE rufus_prepare_device rufus_qt_reporting Qt6::Widgets Qt6::Test)
        add_executable(test_helper_guest tests/vm/test_helper_guest.cpp)
        set_target_properties(test_helper_guest PROPERTIES AUTOMOC ON)
        target_compile_options(test_helper_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_helper_guest PRIVATE Qt6::Widgets Qt6::Test)
        add_executable(test_gpt_guest tests/vm/test_gpt_guest.c)
        target_compile_definitions(test_gpt_guest PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
        target_compile_options(test_gpt_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_gpt_guest PRIVATE rufus_partition_image)
        target_link_options(test_gpt_guest PRIVATE -Wl,--wrap=fsync
            -Wl,--wrap=fdisk_write_disklabel -Wl,--wrap=fdisk_add_partition)
        add_executable(test_usb_guest tests/vm/test_usb_guest.cpp)
        set_target_properties(test_usb_guest PROPERTIES AUTOMOC ON)
        target_link_libraries(test_usb_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_compile_options(test_usb_guest PRIVATE -Wall -Wextra -Wpedantic)
        add_executable(check_usb_mount tests/vm/check_usb_mount.c)
        target_compile_options(check_usb_mount PRIVATE -Wall -Wextra -Wpedantic)
        # Never a host CTest: this opens a real block device inside the fixture VM.
    endif()
    add_executable(test_image_analysis tests/test_image_analysis.cpp)
    set_target_properties(test_image_analysis PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_image_analysis PRIVATE
        RUFUS_BSDTAR="${RUFUS_BSDTAR}" RUFUS_MKISOFS="${RUFUS_MKISOFS}"
        RUFUS_WIM_FIXTURE="$<TARGET_FILE:wim_fixture>"
        RUFUS_MKFS_FAT="${RUFUS_MKFS_FAT}" RUFUS_MCOPY="${RUFUS_MCOPY}" RUFUS_MMD="${RUFUS_MMD}")
    target_compile_options(test_image_analysis PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_image_analysis PRIVATE -Wl,--wrap=AnalyzeIsoImageWithSource
        -Wl,--wrap=OpenWimFromIsoReader -Wl,--wrap=ReadWimMetadata)
    add_dependencies(test_image_analysis wim_fixture)
    target_link_libraries(test_image_analysis PRIVATE rufus_qt_widgets Qt6::Test)
    add_test(NAME rufus_image_analysis_widgets COMMAND test_image_analysis)
    set_tests_properties(rufus_image_analysis_widgets PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;widgets;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(test_image_mode_dialog tests/test_image_mode_dialog.cpp)
    set_target_properties(test_image_mode_dialog PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_image_mode_dialog PRIVATE
        RUFUS_LOCALE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../res/loc/rufus.loc")
    target_compile_options(test_image_mode_dialog PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_image_mode_dialog PRIVATE rufus_qt_widgets Qt6::Test)
    add_test(NAME rufus_image_mode_dialog COMMAND test_image_mode_dialog)
    add_test(NAME rufus_image_mode_dialog_scaled COMMAND test_image_mode_dialog keyboardAndLargeFontLayout)
    set_tests_properties(rufus_image_mode_dialog rufus_image_mode_dialog_scaled PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;widgets;image;no-hardware" TIMEOUT 30)
    set_property(TEST rufus_image_mode_dialog_scaled APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")
    add_executable(test_rufus_reporting
        tests/test_rufus_reporting.cpp
        tests/c_reporting_caller.c
    )
    set_target_properties(test_rufus_reporting PROPERTIES AUTOMOC ON)
    target_compile_options(test_rufus_reporting PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_rufus_reporting PRIVATE
        rufus_qt_reporting Qt6::Test Qt6::Widgets
    )
    add_test(NAME rufus_reporting COMMAND test_rufus_reporting)
    set_tests_properties(rufus_reporting PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "component;widgets;no-hardware"
        TIMEOUT 30
    )

    add_executable(test_rufus_window tests/test_rufus_window.cpp)
    set_target_properties(test_rufus_window PROPERTIES AUTOMOC ON)
    target_link_libraries(test_rufus_window PRIVATE rufus_qt_widgets Qt6::Test)
    target_compile_options(test_rufus_window PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_window COMMAND test_rufus_window)
    add_test(NAME rufus_window_scaled COMMAND test_rufus_window layoutFitsLargeFonts)
    set_tests_properties(rufus_window rufus_window_scaled PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;widgets;no-hardware"
        TIMEOUT 30
    )
    set_property(TEST rufus_window_scaled APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")

    find_program(RUFUS_DBUS_DAEMON dbus-daemon REQUIRED)
    # Explicitly opt-in host smoke check; never run by CTest.
    add_executable(udisks_probe tests/udisks_probe.cpp)
    set_target_properties(udisks_probe PROPERTIES AUTOMOC ON)
    target_link_libraries(udisks_probe PRIVATE rufus_udisks)
    target_compile_options(udisks_probe PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(udisks_fixture tests/udisks_fixture.cpp)
    target_link_libraries(udisks_fixture PRIVATE rufus_udisks)
    target_compile_options(udisks_fixture PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(test_udisks_client tests/test_udisks_client.cpp)
    add_executable(test_udisks_target tests/test_udisks_target.cpp)
    set_target_properties(test_udisks_target PROPERTIES AUTOMOC ON)
    target_link_libraries(test_udisks_target PRIVATE rufus_raw_target Qt6::Test)
    target_compile_definitions(test_udisks_target PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_DBUS_DAEMON="${RUFUS_DBUS_DAEMON}")
    target_compile_options(test_udisks_target PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_udisks_target PRIVATE -Wl,--wrap=fstat -Wl,--wrap=fstat64 -Wl,--wrap=ioctl)
    add_dependencies(test_udisks_target udisks_fixture)
    add_test(NAME rufus_udisks_target COMMAND test_udisks_target)
    set_tests_properties(rufus_udisks_target PROPERTIES LABELS "functional;dbus;write;no-hardware" TIMEOUT 45)
    add_executable(test_raw_write_widgets tests/test_raw_write_widgets.cpp)
    set_target_properties(test_raw_write_widgets PROPERTIES AUTOMOC ON)
    target_link_libraries(test_raw_write_widgets PRIVATE rufus_qt_widgets Qt6::Test)
    target_compile_definitions(test_raw_write_widgets PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_DBUS_DAEMON="${RUFUS_DBUS_DAEMON}")
    target_compile_options(test_raw_write_widgets PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_raw_write_widgets PRIVATE -Wl,--wrap=fstat -Wl,--wrap=fstat64 -Wl,--wrap=ioctl
        -Wl,--wrap=pread -Wl,--wrap=pread64 -Wl,--wrap=pwrite -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    add_dependencies(test_raw_write_widgets udisks_fixture)
    add_test(NAME rufus_raw_write_widgets COMMAND test_raw_write_widgets)
    set_tests_properties(rufus_raw_write_widgets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;dbus;write;widgets;no-hardware" TIMEOUT 60)
    set_target_properties(test_udisks_client PROPERTIES AUTOMOC ON)
    target_link_libraries(test_udisks_client PRIVATE rufus_udisks rufus_qt_widgets Qt6::Test)
    target_compile_definitions(test_udisks_client PRIVATE RUFUS_DBUS_DAEMON="${RUFUS_DBUS_DAEMON}")
    target_compile_options(test_udisks_client PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(test_udisks_client udisks_fixture)
    add_test(NAME rufus_udisks COMMAND test_udisks_client)
    set_tests_properties(rufus_udisks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;dbus;no-hardware"
        TIMEOUT 60
    )
endif()
