# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(RufusLinux LANGUAGES C CXX)

# Keep the preview's upstream version tied to the original Windows resource.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../src/rufus.rc")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../src/rufus.rc" RUFUS_VERSION_RESOURCE
    REGEX "^[ \t]*FILEVERSION [0-9]+,[0-9]+,[0-9]+,[0-9]+")
string(REGEX REPLACE ".*FILEVERSION ([0-9]+),([0-9]+),([0-9]+),.*" "\\1.\\2.\\3"
    RUFUS_UPSTREAM_VERSION "${RUFUS_VERSION_RESOURCE}")
if(NOT RUFUS_UPSTREAM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Cannot read the upstream Rufus version")
endif()
set(RUFUS_PREVIEW_REVISION 47)
set(RUFUS_LINUX_VERSION "${RUFUS_UPSTREAM_VERSION}-linux-preview.${RUFUS_PREVIEW_REVISION}")
configure_file(qt/build_info.h.in generated/rufus_build_info.h @ONLY)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core)
message(STATUS "Building with Qt ${Qt6Core_VERSION}")
option(RUFUS_BUILD_GUI "Build the Qt Widgets application" ON)
option(RUFUS_BUILD_VM_TEST "Build the guarded, guest-only native USB test" OFF)
option(RUFUS_COMPARE_SYSTEM_WIMLIB "Compare the extended upstream reader with a system wimlib in tests" OFF)
include(CTest)
include(GNUInstallDirs)
set(RUFUS_SYSLINUX_UPSTREAM "" CACHE PATH "Optional upstream syslinux-6.04-pre1 release source tree")
set(RUFUS_SYSLINUX4_UPSTREAM "" CACHE PATH "Optional upstream syslinux-4.07 release source tree")
if(RUFUS_SYSLINUX_UPSTREAM OR RUFUS_SYSLINUX4_UPSTREAM)
    add_subdirectory(syslinux)
endif()
if(RUFUS_BUILD_VM_TEST AND NOT BUILD_TESTING)
    message(FATAL_ERROR "RUFUS_BUILD_VM_TEST requires BUILD_TESTING=ON")
endif()

add_library(rufus_qt_reporting STATIC
    qt/job_priority.cpp qt/job_priority.h
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
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    add_library(rufus_download_signature STATIC platform/download_signature.c platform/download_signature.h
        ../src/download_key.h)
    target_compile_features(rufus_download_signature PRIVATE c_std_11)
    target_include_directories(rufus_download_signature PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/platform"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_options(rufus_download_signature PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_download_signature PUBLIC OpenSSL::Crypto)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Network)
    add_library(rufus_http_download STATIC qt/http_download.cpp qt/http_download.h)
    set_target_properties(rufus_http_download PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_http_download PUBLIC cxx_std_17)
    target_include_directories(rufus_http_download PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_http_download PUBLIC Qt6::Network)
    target_compile_options(rufus_http_download PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_signed_download STATIC qt/signed_download.cpp qt/signed_download.h)
    set_target_properties(rufus_signed_download PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_signed_download PUBLIC cxx_std_17)
    target_include_directories(rufus_signed_download PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_options(rufus_signed_download PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_signed_download PUBLIC rufus_http_download PRIVATE rufus_download_signature)
    add_library(rufus_target_io STATIC platform/target_io.c platform/target_io.h)
    target_compile_definitions(rufus_target_io PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_features(rufus_target_io PRIVATE c_std_11)
    target_include_directories(rufus_target_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/platform")
    target_compile_options(rufus_target_io PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_native_disk_facts STATIC platform/native_disk_facts.cpp platform/native_disk_facts.h platform/device_exclusions.cpp platform/device_exclusions.h)
    target_compile_features(rufus_native_disk_facts PRIVATE cxx_std_17)
    target_compile_definitions(rufus_native_disk_facts PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_include_directories(rufus_native_disk_facts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_native_disk_facts PUBLIC Qt6::Core rufus_hdd_score rufus_backing ZLIB::ZLIB)
    target_compile_options(rufus_native_disk_facts PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_hdd_score STATIC ../src/hdd_score.c ../src/hdd_score.h)
    target_compile_features(rufus_hdd_score PRIVATE c_std_11)
    target_compile_definitions(rufus_hdd_score PRIVATE _GNU_SOURCE)
    target_compile_options(rufus_hdd_score PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_hdd_score BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform")
    target_include_directories(rufus_hdd_score PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    add_library(rufus_progress_policy STATIC ../src/progress_policy.c ../src/progress_policy.h)
    target_compile_features(rufus_progress_policy PRIVATE c_std_11)
    target_compile_options(rufus_progress_policy PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_progress_policy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    add_library(rufus_format_policy STATIC ../src/format_policy.c ../src/format_policy.h)
    target_compile_features(rufus_format_policy PRIVATE c_std_11)
    target_compile_options(rufus_format_policy PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_format_policy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_format_policy PUBLIC m)
    add_library(rufus_application STATIC qt/application_storage.cpp qt/application_storage.h
        qt/startup_options.cpp qt/startup_options.h qt/application_instance.cpp qt/application_instance.h
        qt/device_policy.cpp qt/device_policy.h qt/application_cache.cpp qt/application_log.cpp qt/application_log.h qt/device_size.cpp qt/device_size.h)
    target_compile_features(rufus_application PUBLIC cxx_std_17)
    target_compile_definitions(rufus_application PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_include_directories(rufus_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_options(rufus_application PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_application PUBLIC Qt6::Core)
    add_library(rufus_unattend STATIC ../src/unattend.c ../src/unattend.h ../src/strarray.c)
    target_compile_features(rufus_unattend PRIVATE c_std_11)
    target_compile_definitions(rufus_unattend PRIVATE _GNU_SOURCE)
    target_compile_options(rufus_unattend PRIVATE -Wall -Wextra -Wpedantic
        -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/crt_strings.h")
    target_include_directories(rufus_unattend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    add_library(rufus_dos_locale STATIC ../src/dos_locale_policy.c ../src/dos_locale_policy.h)
    target_compile_features(rufus_dos_locale PRIVATE c_std_11)
    target_compile_options(rufus_dos_locale PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_dos_locale PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    add_library(rufus_freedos_files STATIC ../src/freedos_files.c ../src/freedos_files.h ../src/freedos_files.inc)
    target_compile_features(rufus_freedos_files PRIVATE c_std_11)
    target_include_directories(rufus_freedos_files PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    add_library(rufus_media_appearance STATIC platform/media_appearance.cpp platform/media_appearance_policy.cpp platform/media_appearance.h platform/media_appearance.qrc)
    set_target_properties(rufus_media_appearance PROPERTIES AUTORCC ON)
    target_compile_definitions(rufus_media_appearance PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_media_appearance PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_media_appearance PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(rufus_media_appearance PUBLIC Qt6::Core)
    add_library(rufus_freedos_copy STATIC platform/msdos_image.cpp platform/msdos_image.h ../src/msdos_files.c ../src/msdos_files.h platform/freedos_copy.cpp platform/freedos_copy.h)
    target_compile_features(rufus_freedos_copy PRIVATE cxx_std_17)
    target_compile_definitions(rufus_freedos_copy PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_freedos_copy PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_freedos_copy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_freedos_copy PUBLIC rufus_qt_reporting rufus_media_appearance Qt6::Core rufus_freedos_files rufus_target_io rufus_dos_locale)
    set(freedos_manifest "${CMAKE_CURRENT_SOURCE_DIR}/../src/freedos_files.inc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${freedos_manifest}")
    file(STRINGS "${freedos_manifest}" freedos_entries REGEX "^FREEDOS_FILE_ENTRY")
    set(freedos_payloads)
    foreach(entry IN LISTS freedos_entries)
        string(REGEX MATCH "\"([A-Z0-9.]+)\"" matched_name "${entry}")
        if(NOT matched_name)
            message(FATAL_ERROR "Invalid FreeDOS manifest row: ${entry}")
        endif()
        list(APPEND freedos_payloads "${CMAKE_CURRENT_SOURCE_DIR}/../res/freedos/${CMAKE_MATCH_1}")
    endforeach()
    qt_add_resources(rufus_freedos_copy freedos PREFIX "/rufus/freedos"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/../res/freedos" FILES ${freedos_payloads})
    add_library(rufus_windows_customization STATIC platform/windows_customization.cpp platform/windows_customization.h
        platform/windows_install_options.cpp platform/windows_install_options.h
        platform/windows_region_options.cpp platform/windows_region_options.h
        platform/windows_bootloaders.cpp platform/windows_bootloaders.h
        platform/windows_upgrade.cpp platform/windows_upgrade.h platform/windows_setup.qrc)
    set_target_properties(rufus_windows_customization PROPERTIES AUTORCC ON)
    target_compile_features(rufus_windows_customization PRIVATE cxx_std_17)
    target_compile_options(rufus_windows_customization PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(rufus_windows_customization PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_include_directories(rufus_windows_customization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_customization PUBLIC rufus_unattend Qt6::Core PRIVATE
        rufus_windows_wim rufus_windows_registry rufus_config_patch rufus_iso_scan rufus_windows_bcd rufus_target_io rufus_prepare_process)
    add_library(rufus_config_patch STATIC platform/config_patch.cpp platform/config_patch.h
        ../src/config_text.c ../src/config_text.h)
    target_compile_features(rufus_config_patch PRIVATE c_std_11 cxx_std_17)
    target_compile_definitions(rufus_config_patch PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_config_patch PRIVATE -Wall -Wextra -Wpedantic
        -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/crt_strings.h")
    target_include_directories(rufus_config_patch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_config_patch PUBLIC Qt6::Core)
    add_library(rufus_winpe_prepare STATIC platform/winpe_prepare.cpp platform/winpe_prepare.h
        ../src/winpe_setup.c ../src/winpe_setup.h)
    target_compile_features(rufus_winpe_prepare PRIVATE c_std_11 cxx_std_17)
    target_compile_definitions(rufus_winpe_prepare PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_winpe_prepare PRIVATE -Wall -Wextra -Wpedantic
        -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/crt_strings.h")
    target_link_libraries(rufus_winpe_prepare PUBLIC rufus_config_patch)
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
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../src/iso_bootloaders.c"
        PROPERTIES COMPILE_DEFINITIONS ScanGrubAndEfiBootloaders=ScanOriginalGrubAndEfiBootloaders)
    target_sources(rufus_iso_scan PRIVATE platform/bootloader_scan.c)
    target_compile_features(rufus_iso_scan PRIVATE c_std_11)
    target_compile_definitions(rufus_iso_scan PRIVATE _GNU_SOURCE)
    target_compile_options(rufus_iso_scan PRIVATE -Wall -Wextra -Wpedantic
        -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/crt_strings.h")
    target_include_directories(rufus_iso_scan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_iso_scan PUBLIC bundled_cdio bundled_fat)
    add_library(rufus_iso_links STATIC platform/iso_links.cpp platform/iso_links.h)
    target_compile_features(rufus_iso_links PRIVATE cxx_std_17)
    target_compile_definitions(rufus_iso_links PRIVATE _GNU_SOURCE)
    target_link_libraries(rufus_iso_links PUBLIC rufus_image_source Qt6::Core)
    add_library(rufus_syslinux_native STATIC platform/syslinux_native.cpp platform/syslinux_native.h
        platform/syslinux_source.cpp platform/syslinux_source.h platform/legacy_iso.cpp platform/legacy_iso.h platform/syslinux_resources.cpp platform/syslinux_resources.h platform/syslinux_boot.qrc)
    set_target_properties(rufus_syslinux_native PROPERTIES AUTORCC ON)
    target_compile_features(rufus_syslinux_native PRIVATE cxx_std_17)
    target_compile_options(rufus_syslinux_native PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_syslinux_native PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_syslinux_native PUBLIC Qt6::Core rufus_iso_scan rufus_iso_links)
    add_library(rufus_image_source STATIC platform/image_source.c)
    target_compile_definitions(rufus_image_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_image_source PRIVATE -Wall -Wextra)
    target_include_directories(rufus_image_source PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_image_source PUBLIC rufus_iso_scan)
    find_package(ZLIB REQUIRED)
    add_library(rufus_bled_zstd STATIC
        ../src/bled/fse_decompress.c ../src/bled/huf_decompress.c ../src/bled/xxhash.c
        ../src/bled/zstd_common.c ../src/bled/zstd_decompress.c
        ../src/bled/zstd_decompress_block.c ../src/bled/zstd_ddict.c
        ../src/bled/zstd_entropy_common.c ../src/bled/zstd_error_private.c)
    target_compile_features(rufus_bled_zstd PRIVATE c_std_11)
    target_compile_definitions(rufus_bled_zstd PRIVATE RUFUS_NATIVE_ZSTD)
    target_include_directories(rufus_bled_zstd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_options(rufus_bled_zstd PRIVATE -Wall -Wextra)
    add_library(rufus_compressed_source STATIC platform/compressed_source.c platform/compressed_source.h
        platform/compressed_bzip2.c platform/compressed_bzip2.h ../src/bled/bunzip_core.inc
        platform/compressed_lzma.c platform/compressed_lzma.h ../src/bled/unlzma_core.inc
        platform/compressed_lzw.c platform/compressed_lzw.h ../src/bled/uncompress_core.inc
        platform/compressed_zip.c platform/compressed_zip.h ../src/bled/unzip_format.inc
        platform/compressed_xz.c platform/compressed_xz.h
        platform/compressed_zstd.c platform/compressed_zstd.h
        platform/compressed_vtsi.c platform/compressed_vtsi.h
		platform/compressed_vhd.c platform/compressed_vhd.h
        platform/compressed_vhdx.c platform/compressed_vhdx.h
        platform/virtual_disk.c platform/virtual_disk.h platform/vhdx_replay.c
        platform/compressed_ffu.c platform/compressed_ffu.h platform/ffu_codec.c platform/ffu_codec.h
        platform/ffu_catalog.c platform/ffu_catalog.h platform/ffu_production_root.inc
        platform/ffu_manifest.c platform/ffu_manifest.h
        ../src/image_compression.h ../src/bled/compression.h)
    target_compile_features(rufus_compressed_source PRIVATE c_std_11)
    target_compile_definitions(rufus_compressed_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_compressed_source PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_compressed_source PUBLIC rufus_image_source ZLIB::ZLIB rufus_hash_core rufus_bled_zstd OpenSSL::Crypto)
    add_library(rufus_image_save STATIC platform/image_save.cpp platform/image_save.h platform/ffu_catalog.inc
        platform/image_save_destination.cpp platform/image_save_destination.h)
    target_compile_definitions(rufus_image_save PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_image_save PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_image_save PUBLIC rufus_compressed_source Qt6::Core)
    find_program(RUFUS_MKISOFS NAMES mkisofs)
    if(NOT RUFUS_MKISOFS)
        set(RUFUS_MKISOFS "/usr/bin/mkisofs" CACHE FILEPATH "Optional cdrtools UDF encoder" FORCE)
    endif()
    add_library(rufus_iso_export STATIC platform/iso_export.cpp platform/iso_export.h)
    target_compile_definitions(rufus_iso_export PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_iso_export PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_iso_export PUBLIC rufus_image_save rufus_iso_scan Qt6::Core)
    find_package(Threads REQUIRED)
    add_library(rufus_hash_core STATIC platform/hash_core.c platform/hash_native.h ../src/hash_core.h ../src/hash_core.inc)
    target_compile_features(rufus_hash_core PRIVATE c_std_11)
    target_include_directories(rufus_hash_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_options(rufus_hash_core PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_hash_core PUBLIC Threads::Threads)
    add_library(rufus_image_hash STATIC platform/image_hash.c platform/image_hash.h)
    target_compile_features(rufus_image_hash PRIVATE c_std_11)
    target_compile_definitions(rufus_image_hash PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_image_hash PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_image_hash PUBLIC rufus_hash_core rufus_image_source Threads::Threads)
    add_library(rufus_embedded_efi_files STATIC platform/embedded_efi_files.cpp platform/embedded_efi_files.h)
    target_include_directories(rufus_embedded_efi_files PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_embedded_efi_files PUBLIC rufus_image_source Qt6::Core)
    target_compile_options(rufus_embedded_efi_files PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_secure_boot STATIC platform/secure_boot.cpp platform/secure_boot.h platform/secure_boot.qrc
        platform/secure_boot_cache.cpp platform/secure_boot_cache.h)
    set_target_properties(rufus_secure_boot PROPERTIES AUTORCC ON)
    target_compile_features(rufus_secure_boot PUBLIC cxx_std_17)
    target_include_directories(rufus_secure_boot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_secure_boot PUBLIC rufus_embedded_efi_files rufus_hash_core rufus_iso_scan rufus_qt_reporting Qt6::Core OpenSSL::Crypto)
    target_compile_options(rufus_secure_boot PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_media_integrity STATIC platform/media_integrity.cpp platform/media_integrity.h platform/media_integrity.qrc)
    set_target_properties(rufus_media_integrity PROPERTIES AUTORCC ON)
    target_compile_features(rufus_media_integrity PRIVATE cxx_std_17)
    target_compile_definitions(rufus_media_integrity PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_media_integrity PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_media_integrity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_media_integrity PUBLIC rufus_hash_core rufus_image_source rufus_iso_scan Qt6::Core)
    add_library(rufus_badblocks STATIC platform/badblocks_test.c platform/badblocks_test.h ../src/badblocks_policy.h)
    target_compile_definitions(rufus_badblocks PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_badblocks PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_badblocks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_badblocks PUBLIC rufus_raw_write)
    add_library(rufus_prepare_badblocks STATIC platform/prepare_badblocks.cpp platform/prepare_badblocks.h)
    target_link_libraries(rufus_prepare_badblocks PUBLIC rufus_badblocks Qt6::Core)
    add_library(rufus_raw_write STATIC platform/raw_write.c platform/zero_write.c)
    target_compile_definitions(rufus_raw_write PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(rufus_raw_write PUBLIC rufus_target_io)
    target_compile_options(rufus_raw_write PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_raw_write PUBLIC rufus_image_source)
    add_library(rufus_compressed_write STATIC platform/compressed_write.c platform/compressed_write.h platform/compressed_geometry.c)
    target_compile_features(rufus_compressed_write PRIVATE c_std_11)
    target_compile_definitions(rufus_compressed_write PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_compressed_write PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_compressed_write PUBLIC rufus_compressed_source rufus_hash_core rufus_target_io)
    add_library(rufus_iso_extract STATIC platform/iso_extract.c platform/iso_extract.h)
    target_compile_definitions(rufus_iso_extract PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_iso_extract PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_iso_extract PUBLIC rufus_iso_links rufus_embedded_efi_files rufus_image_source rufus_config_patch rufus_target_io rufus_hash_core)
    add_library(rufus_format_command STATIC platform/format_command.c platform/format_command.h)
    target_compile_features(rufus_format_command PRIVATE c_std_11)
    target_compile_definitions(rufus_format_command PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_format_command PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_ntfs_format STATIC platform/ntfs_format.cpp platform/ntfs_format.h)
    target_compile_features(rufus_ntfs_format PRIVATE cxx_std_17)
    target_compile_definitions(rufus_ntfs_format PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(rufus_ntfs_format PUBLIC rufus_target_io)
    target_compile_options(rufus_ntfs_format PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_ntfs_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_ntfs_format PUBLIC rufus_format_command rufus_format_policy Qt6::Core)
    pkg_check_modules(EXT2FS REQUIRED IMPORTED_TARGET ext2fs)
    find_path(RUFUS_EXT2FS_HEADERS NAMES ext2fs/ext2_fs.h REQUIRED)
    add_library(rufus_ext_format STATIC platform/ext_format.cpp platform/ext_format.h)
    target_compile_features(rufus_ext_format PRIVATE cxx_std_17)
    target_compile_definitions(rufus_ext_format PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_ext_format PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_ext_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${RUFUS_EXT2FS_HEADERS}")
    target_link_libraries(rufus_ext_format PUBLIC PkgConfig::EXT2FS rufus_target_io rufus_format_command rufus_format_policy Qt6::Core)
    add_library(rufus_udf_format STATIC platform/udf_format.cpp platform/udf_format.h)
    target_compile_features(rufus_udf_format PRIVATE cxx_std_17)
    target_compile_definitions(rufus_udf_format PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_udf_format PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_udf_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_udf_format PUBLIC rufus_target_io rufus_raw_write rufus_format_command Qt6::Core)
    find_program(RUFUS_MKUDFFS mkudffs)
    if(NOT RUFUS_MKUDFFS)
        set(RUFUS_MKUDFFS "/usr/bin/mkudffs" CACHE FILEPATH "Optional native UDF formatter" FORCE)
    endif()
    target_compile_definitions(rufus_udf_format PUBLIC RUFUS_MKUDFFS_PATH="${RUFUS_MKUDFFS}")
    add_library(rufus_fat_format STATIC platform/fat_format.c platform/fat_format.h)
    target_compile_features(rufus_fat_format PRIVATE c_std_11)
    target_compile_definitions(rufus_fat_format PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(rufus_fat_format PUBLIC rufus_target_io rufus_raw_write rufus_format_policy)
    target_compile_options(rufus_fat_format PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_fat_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_fat_format PUBLIC bundled_fat rufus_format_command)
    # Bounded in-memory stdio callbacks shared by Rufus's original ms-sys
    # FAT and NTFS record writers. They never receive a target descriptor.
    add_library(rufus_ms_sys_memory STATIC platform/ms_sys_memory.c)
    target_compile_features(rufus_ms_sys_memory PRIVATE c_std_11)
    target_compile_definitions(rufus_ms_sys_memory PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_ms_sys_memory PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_ms_sys_memory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    # Original Rufus FAT writers with bounded native staging I/O. FreeDOS is
    # qualified on files/devices; standard records are qualified on files.
    add_library(rufus_fat_boot STATIC platform/fat_boot.c platform/fat_boot.h
        ../src/ms-sys/fat16.c ../src/ms-sys/fat32.c ../src/ms-sys/partition_info.c)
    target_compile_features(rufus_fat_boot PRIVATE c_std_11)
    target_compile_definitions(rufus_fat_boot PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_fat_boot PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_fat_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/ms-sys/inc")
    target_link_libraries(rufus_fat_boot PUBLIC rufus_fat_format rufus_ms_sys_memory rufus_format_policy)
    add_library(rufus_ntfs_boot STATIC platform/ntfs_boot.cpp platform/ntfs_boot.h ../src/ms-sys/ntfs.c)
    target_compile_features(rufus_ntfs_boot PRIVATE c_std_11 cxx_std_17)
    target_compile_definitions(rufus_ntfs_boot PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_ntfs_boot PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_ntfs_boot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../src/ms-sys/inc")
    target_link_libraries(rufus_ntfs_boot PUBLIC rufus_ntfs_format PRIVATE rufus_ms_sys_memory)
    # Reconstruct installed bytes with Rufus's patcher, using only pinned
    # templates. Both payloads have 192 extent slots, sufficient even for
    # completely fragmented loaders (72 sectors for v4, 134 for v6).
    set(syslinux4_sys_hash "d5b6310740a596ba4367719a3dff4b3442f6da81cbeafbb0bf3fab026d8bc910")
    set(syslinux4_bss_hash "8dc6ec58b4f1df391db68d9e2165ccd47d84f4ba64675f87b677077aae5b53fa")
    set(syslinux6_sys_hash "73b62767a16200b9af193a7d5c94e9c294c6dbb6d5b17c15038c9f3173c9a7bc")
    set(syslinux6_bss_hash "cc40ba0349782cb4c9021e54dcc0a4540c3a8b96088b3a5648671926ef44d2f0")
    foreach(major 4 6)
        foreach(payload IN ITEMS sys bss)
            get_filename_component(payload_file
                "${CMAKE_CURRENT_SOURCE_DIR}/../res/syslinux/ldlinux_v${major}.${payload}" ABSOLUTE)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${payload_file}")
            file(SHA256 "${payload_file}" payload_hash)
            if(NOT payload_hash STREQUAL syslinux${major}_${payload}_hash)
                message(FATAL_ERROR "Syslinux verifier needs the qualified v${major} ${payload} payload")
            endif()
        endforeach()
    endforeach()
    add_library(rufus_syslinux_patch STATIC ../src/syslinux/libinstaller/syslxmod.c
        ../src/syslinux/libinstaller/fs.c ../src/syslinux/libinstaller/setadv.c)
    target_compile_features(rufus_syslinux_patch PRIVATE c_std_11)
    add_library(rufus_syslinux_install STATIC platform/syslinux_install.cpp platform/syslinux_install.h
        platform/syslinux_payload.cpp platform/syslinux_payload.h
        platform/syslinux_ntfs.cpp platform/syslinux_ntfs.h)
    set_target_properties(rufus_syslinux_install PROPERTIES AUTORCC ON)
    target_compile_features(rufus_syslinux_install PRIVATE cxx_std_17)
    target_compile_definitions(rufus_syslinux_install PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_syslinux_install PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_syslinux_install PUBLIC rufus_syslinux_native rufus_image_source rufus_partition_image
        rufus_fat_format rufus_ntfs_format PRIVATE rufus_syslinux_patch)
    find_program(RUFUS_NATIVE_MCOPY mcopy)
    find_program(RUFUS_NATIVE_MATTRIB mattrib)
    set(RUFUS_MTOOLS_DIRECTORY "/usr/bin")
    if(RUFUS_NATIVE_MCOPY AND RUFUS_NATIVE_MATTRIB)
        get_filename_component(RUFUS_MTOOLS_DIRECTORY "${RUFUS_NATIVE_MCOPY}" DIRECTORY)
        get_filename_component(mattrib_directory "${RUFUS_NATIVE_MATTRIB}" DIRECTORY)
        if(NOT mattrib_directory STREQUAL RUFUS_MTOOLS_DIRECTORY)
            message(FATAL_ERROR "Native Syslinux requires mcopy and mattrib in the same trusted tools directory")
        endif()
    endif()
    target_compile_definitions(rufus_syslinux_install PUBLIC
        RUFUS_NATIVE_SYSLINUX_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-syslinux"
        RUFUS_NATIVE_SYSLINUX4_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-syslinux4"
        RUFUS_NATIVE_EXTLINUX_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-extlinux"
        RUFUS_MTOOLS_DIRECTORY="${RUFUS_MTOOLS_DIRECTORY}")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RUFUS_FDISK REQUIRED IMPORTED_TARGET fdisk>=2.35)
    pkg_check_modules(RUFUS_BLKID REQUIRED IMPORTED_TARGET blkid>=2.35)
    pkg_check_modules(RUFUS_MOUNT REQUIRED IMPORTED_TARGET mount>=2.35)
    add_library(rufus_backing STATIC platform/application_devices.cpp platform/application_devices.h)
    target_compile_definitions(rufus_backing PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_backing PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_backing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_backing PUBLIC Qt6::Core PRIVATE PkgConfig::RUFUS_MOUNT)
    add_library(rufus_target_mounts STATIC platform/target_mounts.cpp platform/target_mounts.h)
    target_compile_features(rufus_target_mounts PRIVATE cxx_std_17)
    target_compile_definitions(rufus_target_mounts PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_target_mounts PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_target_mounts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_target_mounts PUBLIC Qt6::Core rufus_backing PRIVATE PkgConfig::RUFUS_MOUNT)
    add_library(rufus_exfat_format STATIC platform/exfat_format.cpp platform/exfat_format.h)
    find_program(RUFUS_MKFS_EXFAT NAMES mkfs.exfat)
    set(exfat_program "")
    if(RUFUS_MKFS_EXFAT)
        set(exfat_program "${RUFUS_MKFS_EXFAT}")
    endif()
    target_compile_definitions(rufus_exfat_format PUBLIC RUFUS_MKFS_EXFAT_PATH="${exfat_program}")
    target_compile_features(rufus_exfat_format PRIVATE cxx_std_17)
    target_compile_definitions(rufus_exfat_format PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(rufus_exfat_format PUBLIC rufus_target_io)
    target_compile_options(rufus_exfat_format PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_exfat_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_exfat_format PUBLIC rufus_format_command rufus_format_policy Qt6::Core PRIVATE PkgConfig::RUFUS_BLKID)

    pkg_check_modules(RUFUS_HIVEX REQUIRED IMPORTED_TARGET hivex)
    add_library(rufus_windows_registry STATIC platform/windows_registry.c platform/windows_registry.h)
    target_compile_features(rufus_windows_registry PRIVATE c_std_11)
    target_compile_definitions(rufus_windows_registry PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_windows_registry PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_windows_registry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_registry PUBLIC rufus_unattend PkgConfig::RUFUS_HIVEX)
    add_library(rufus_windows_bcd STATIC platform/windows_bcd.cpp platform/windows_bcd.h)
    target_compile_features(rufus_windows_bcd PRIVATE cxx_std_17)
    target_compile_definitions(rufus_windows_bcd PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_windows_bcd PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_windows_bcd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_bcd PUBLIC Qt6::Core PRIVATE PkgConfig::RUFUS_HIVEX)
    add_library(rufus_partition_layout STATIC ../src/partition_layout.c)
    target_include_directories(rufus_partition_layout PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_features(rufus_partition_layout PRIVATE c_std_11)
    target_compile_options(rufus_partition_layout PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_partition_image STATIC platform/partition_image.c platform/partition_image.h)
    target_compile_features(rufus_partition_image PRIVATE c_std_11)
    target_compile_definitions(rufus_partition_image PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_include_directories(rufus_partition_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_partition_image PUBLIC rufus_target_io)
    target_compile_options(rufus_partition_image PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_partition_image PUBLIC rufus_partition_layout PkgConfig::RUFUS_FDISK)
    add_library(rufus_uefi_ntfs STATIC platform/uefi_ntfs.c platform/uefi_ntfs.h)
    target_compile_features(rufus_uefi_ntfs PRIVATE c_std_11)
    target_compile_definitions(rufus_uefi_ntfs PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_uefi_ntfs PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_uefi_ntfs PUBLIC rufus_raw_write rufus_partition_image)
    find_program(RUFUS_MKFS_FAT mkfs.fat REQUIRED)
    find_program(RUFUS_MKNTFS NAMES mkntfs mkfs.ntfs REQUIRED)
    find_program(RUFUS_NTFSRESIZE NAMES ntfsresize)
    if(NOT RUFUS_NTFSRESIZE)
        set(RUFUS_NTFSRESIZE "/usr/bin/ntfsresize" CACHE FILEPATH "Optional optimized FFU filesystem resize provider" FORCE)
    endif()
    find_program(RUFUS_MKE2FS mke2fs REQUIRED)
    find_program(RUFUS_NTFS_3G NAMES lowntfs-3g)
    if(NOT RUFUS_NTFS_3G)
        set(RUFUS_NTFS_3G "/usr/bin/lowntfs-3g" CACHE FILEPATH "Optional NTFS-3G fallback with ignore_case support" FORCE)
    endif()
    file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/../res/uefi/uefi-ntfs.img" RUFUS_UEFI_NTFS_BYTES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../res/uefi/uefi-ntfs.img")
    add_library(rufus_prepare_process STATIC qt/prepare_image_protocol.cpp qt/prepare_image_protocol.h
        qt/prepare_image_process.cpp qt/prepare_image_process.h)
    set_target_properties(rufus_prepare_process PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_prepare_process PUBLIC cxx_std_17)
    target_include_directories(rufus_prepare_process PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_prepare_process PUBLIC rufus_syslinux_native rufus_freedos_copy rufus_media_appearance rufus_format_policy rufus_progress_policy rufus_dos_locale rufus_compressed_write rufus_application Qt6::Core)
    target_compile_options(rufus_prepare_process PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(rufus-prepare-helper helper/prepare_main.cpp helper/boot_resources.qrc)
    set_target_properties(rufus-prepare-helper PROPERTIES AUTOMOC ON AUTORCC ON)
    target_compile_definitions(rufus-prepare-helper PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
        RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
        RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_compile_options(rufus-prepare-helper PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_prepare_process rufus_qt_reporting
        rufus_partition_image rufus_fat_format rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
    add_subdirectory(wimlib)
    target_sources(rufus_iso_extract PRIVATE platform/iso_wim_split.c platform/iso_wim_split.h)
    target_link_libraries(rufus_iso_extract PUBLIC rufus_wimlib)
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
    add_library(rufus_windows_wim STATIC platform/windows_wim.c platform/windows_wim.h)
    target_compile_features(rufus_windows_wim PRIVATE c_std_11)
    target_compile_definitions(rufus_windows_wim PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_windows_wim PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_windows_wim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_wim PUBLIC rufus_wimlib)
    add_library(rufus_windows_apply STATIC platform/windows_apply.cpp platform/windows_apply.h)
    target_compile_features(rufus_windows_apply PRIVATE cxx_std_17)
    target_compile_definitions(rufus_windows_apply PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_windows_apply PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_windows_apply PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_apply PUBLIC rufus_wim_metadata rufus_image_source rufus_target_io Qt6::Core)
    add_library(rufus_windows_image STATIC platform/windows_image.c platform/windows_image.h)
    target_compile_options(rufus_windows_image PRIVATE -Wall -Wextra)
    target_include_directories(rufus_windows_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_image PUBLIC rufus_wim_metadata)
    add_library(rufus_application_updates STATIC qt/application_updates.cpp qt/application_updates.h)
    set_target_properties(rufus_application_updates PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_application_updates PUBLIC cxx_std_17)
    target_include_directories(rufus_application_updates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_application_updates PUBLIC rufus_http_download)
    target_compile_options(rufus_application_updates PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_policy_updates STATIC qt/policy_updates.cpp qt/policy_updates.h)
    set_target_properties(rufus_policy_updates PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_policy_updates PUBLIC cxx_std_17)
    target_include_directories(rufus_policy_updates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_policy_updates PUBLIC rufus_secure_boot rufus_http_download)
    target_compile_options(rufus_policy_updates PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_qt_analysis STATIC
        qt/image_analysis_job.cpp
        qt/image_analysis_job.h
        qt/syslinux_selection.cpp qt/syslinux_selection.h
        qt/grub2_core_selection.cpp
        qt/grub2_core_selection.h
        qt/image_resources.qrc
    )
    set_target_properties(rufus_qt_analysis PROPERTIES AUTOMOC ON AUTORCC ON)
    target_link_libraries(rufus_qt_analysis PUBLIC rufus_windows_image rufus_qt_reporting rufus_image_source rufus_secure_boot)
    target_compile_options(rufus_qt_analysis PRIVATE -Wall -Wextra -Wpedantic)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets DBus Concurrent)
    add_library(rufus_udisks STATIC
        platform/device.h
        platform/udisks_client.cpp
        platform/udisks_client.h
        platform/udisks_content.cpp
        platform/device_content.cpp
        platform/device_content.h
        ../src/drive_label.c
        ../src/drive_label.h
    )
    set_target_properties(rufus_udisks PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_udisks PUBLIC cxx_std_17)
    target_include_directories(rufus_udisks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(rufus_udisks PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(rufus_udisks PUBLIC Qt6::DBus Qt6::Concurrent rufus_native_disk_facts rufus_backing)
    target_compile_options(rufus_udisks PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_device_selection STATIC platform/device_selection.cpp platform/device_selection.h)
    target_compile_definitions(rufus_device_selection PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_device_selection PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_device_selection PUBLIC rufus_udisks rufus_prepare_process)
    add_library(rufus_save_source STATIC platform/save_source.cpp platform/save_source.h)
    set_target_properties(rufus_save_source PROPERTIES AUTOMOC ON)
    target_compile_definitions(rufus_save_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 RUFUS_NTFS_3G_PATH="${RUFUS_NTFS_3G}")
    target_compile_options(rufus_save_source PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_save_source PUBLIC rufus_device_selection rufus_target_mounts rufus_image_save rufus_qt_reporting)
    target_compile_definitions(rufus-prepare-helper PRIVATE RUFUS_SAVE_SOURCE_HELPER)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_save_source)
    add_library(rufus_windows_to_go STATIC platform/windows_to_go.cpp platform/windows_to_go.h)
    target_compile_features(rufus_windows_to_go PRIVATE cxx_std_17)
    target_compile_definitions(rufus_windows_to_go PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_compile_options(rufus_windows_to_go PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_windows_to_go PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_windows_to_go PUBLIC rufus_windows_apply rufus_windows_customization
        rufus_wim_file rufus_prepare_source rufus_format_options PRIVATE rufus_wim_read_file PkgConfig::RUFUS_FDISK)
    target_link_libraries(rufus_qt_analysis PUBLIC rufus_windows_to_go)
    add_library(rufus_prepare_device STATIC platform/prepare_device.cpp platform/prepare_device.h platform/device_operations.cpp platform/device_operations.h)
    target_compile_definitions(rufus_prepare_device PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_NTFS_3G_PATH="${RUFUS_NTFS_3G}")
    target_compile_options(rufus_prepare_device PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_prepare_device PUBLIC rufus_device_selection rufus_fat_format rufus_ntfs_format rufus_ext_format rufus_udf_format
        rufus_ntfs_boot rufus_exfat_format rufus_target_mounts rufus_windows_to_go PRIVATE PkgConfig::RUFUS_FDISK)
    add_library(rufus_prepare_source STATIC platform/prepare_source.cpp platform/prepare_source.h
        platform/zip_extract.cpp platform/zip_extract.h
        platform/grub2_cache.cpp platform/grub2_cache.h platform/grub2_resources.qrc)
    set_target_properties(rufus_prepare_source PROPERTIES AUTORCC ON)
    target_compile_definitions(rufus_prepare_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_prepare_source PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_prepare_source PUBLIC rufus_prepare_process rufus_iso_extract rufus_download_signature)
    target_link_libraries(rufus_prepare_device PUBLIC rufus_prepare_source)
    target_link_libraries(rufus_qt_analysis PUBLIC rufus_prepare_source)
    target_link_libraries(rufus_qt_analysis PUBLIC rufus_signed_download)
    add_library(rufus_iso_write_check STATIC platform/iso_write_check.cpp platform/iso_write_check.h)
    target_compile_definitions(rufus_iso_write_check PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_iso_write_check PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_iso_write_check PUBLIC rufus_prepare_source rufus_partition_image rufus_fat_format rufus_format_policy rufus_ntfs_format rufus_windows_image rufus_windows_customization rufus_syslinux_install PRIVATE rufus_wim_read_file rufus_winpe_prepare)
    target_compile_definitions(rufus_iso_write_check PRIVATE RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_prepare_device)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_prepare_source)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_iso_write_check)
    target_link_libraries(rufus_iso_write_check PUBLIC rufus_media_integrity)
    configure_file(helper/org.rufus.PrepareDevice.policy.in org.rufus.PrepareDevice.policy @ONLY)
    install(TARGETS rufus-prepare-helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.rufus.PrepareDevice.policy"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
    add_library(rufus_raw_target STATIC platform/raw_write_plan.cpp platform/raw_write_plan.h
        platform/udisks_target.cpp platform/udisks_target.h)
    set_target_properties(rufus_raw_target PROPERTIES AUTOMOC ON)
    target_compile_definitions(rufus_raw_target PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_raw_target PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_raw_target PUBLIC rufus_prepare_badblocks rufus_udisks rufus_raw_write rufus_prepare_source)
    add_library(rufus_raw_device STATIC platform/raw_device.cpp platform/raw_device.h
        platform/ffu_finalize.cpp platform/ffu_finalize.h)
    target_compile_definitions(rufus_raw_device PRIVATE RUFUS_NTFSRESIZE_PATH="${RUFUS_NTFSRESIZE}")
    target_compile_options(rufus_raw_device PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_raw_device PUBLIC rufus_prepare_device rufus_raw_target rufus_compressed_write)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_raw_device)
    add_library(rufus_qt_write STATIC qt/raw_write_controller.cpp qt/raw_write_controller.h)
    set_target_properties(rufus_qt_write PROPERTIES AUTOMOC ON)
    target_compile_options(rufus_qt_write PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_qt_write PUBLIC rufus_raw_target rufus_qt_reporting rufus_device_selection)
    add_library(rufus_format_options STATIC platform/format_options.cpp platform/format_options.h)
    target_compile_options(rufus_format_options PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_format_options PUBLIC rufus_prepare_process rufus_format_policy rufus_partition_image rufus_fat_format rufus_ntfs_format rufus_exfat_format rufus_ext_format rufus_udf_format Qt6::Core)
    target_compile_definitions(rufus_format_options PRIVATE RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_include_directories(rufus_format_options PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_iso_write_check PUBLIC rufus_format_options)
    target_link_libraries(rufus-prepare-helper PRIVATE rufus_format_options)
    add_library(rufus_qt_iso_write STATIC qt/iso_write_controller.cpp qt/iso_write_controller.h)
    set_target_properties(rufus_qt_iso_write PROPERTIES AUTOMOC ON)
    target_compile_options(rufus_qt_iso_write PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(rufus_qt_iso_write PRIVATE RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}" RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}"
        RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}")
    target_link_libraries(rufus_qt_iso_write PUBLIC rufus_iso_write_check rufus_device_selection rufus_qt_reporting rufus_format_options)
    target_link_libraries(rufus_qt_iso_write PUBLIC rufus_qt_analysis)
    target_compile_definitions(rufus_qt_iso_write PRIVATE RUFUS_NTFS_3G_PATH="${RUFUS_NTFS_3G}")
    add_library(rufus_qt_hash STATIC qt/image_hash_job.cpp qt/image_hash_job.h
        qt/image_hash_controller.cpp qt/image_hash_controller.h)
    set_target_properties(rufus_qt_hash PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_qt_hash PUBLIC cxx_std_17)
    target_compile_options(rufus_qt_hash PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(rufus_qt_hash PUBLIC rufus_image_hash rufus_prepare_source rufus_qt_reporting)
    add_library(rufus_qt_save STATIC qt/image_save_controller.cpp qt/image_save_controller.h qt/image_save_job.cpp qt/image_save_job.h
        qt/save_source_process.cpp qt/save_source_process.h)
    set_target_properties(rufus_qt_save PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_qt_save PUBLIC cxx_std_17)
    target_compile_options(rufus_qt_save PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(rufus_qt_save PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 RUFUS_MKISOFS_PATH="${RUFUS_MKISOFS}"
        RUFUS_SAVE_HELPER_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-prepare-helper")
    target_link_libraries(rufus_qt_save PUBLIC rufus_iso_export rufus_save_source rufus_qt_reporting)
    add_library(rufus_msdos_selection STATIC qt/msdos_selection.cpp qt/msdos_selection.h)
    set_target_properties(rufus_msdos_selection PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_msdos_selection PUBLIC cxx_std_17)
    target_include_directories(rufus_msdos_selection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rufus_msdos_selection PUBLIC rufus_http_download rufus_freedos_copy)
    add_library(rufus_image_download STATIC
        platform/download_catalog.cpp platform/download_catalog.h
        qt/download_provider.cpp qt/download_provider.h
        qt/image_download.cpp qt/image_download.h
        qt/image_select_button.cpp qt/image_select_button.h
        qt/image_download_dialog.cpp qt/image_download_dialog.h)
    set_target_properties(rufus_image_download PROPERTIES AUTOMOC ON)
    target_compile_features(rufus_image_download PUBLIC cxx_std_17)
    target_compile_definitions(rufus_image_download PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(rufus_image_download PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(rufus_image_download PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_image_download PUBLIC Qt6::Widgets rufus_http_download rufus_signed_download
        rufus_compressed_source rufus_iso_export rufus_qt_reporting)
    add_library(rufus_localization STATIC qt/application_language.cpp qt/application_language.h
        qt/rufus_locales.qrc ../src/locale_text.c ../src/locale_text.h)
    set_target_properties(rufus_localization PROPERTIES AUTORCC ON)
    target_compile_features(rufus_localization PUBLIC cxx_std_17)
    target_include_directories(rufus_localization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(rufus_localization PUBLIC rufus_application Qt6::Widgets)
    target_compile_options(rufus_localization PRIVATE -Wall -Wextra -Wpedantic)
    add_library(rufus_qt_widgets STATIC
        qt/policy_update_dialog.cpp
        qt/policy_update_dialog.h
        qt/about_dialog.cpp
        qt/about_dialog.h
        qt/desktop_controls.cpp qt/desktop_controls.h
        qt/main_window.cpp
        qt/main_window.h
        qt/main_window.ui
        qt/rufus_icons.qrc
        qt/selection_controller.cpp
        qt/selection_controller.h
        qt/image_hash_dialog.cpp
        qt/image_hash_dialog.h
        qt/image_mode_dialog.cpp
        qt/image_mode_dialog.h
        qt/freedos_locale_dialog.cpp
        qt/freedos_locale_dialog.h
        qt/windows_user_dialog.cpp
        qt/windows_user_dialog.h
        qt/windows_image_dialog.cpp
        qt/windows_image_dialog.h
        qt/windows_region_dialog.cpp
        qt/windows_region_dialog.h
        qt/windows_silent_dialog.cpp
        qt/windows_silent_dialog.h
    )
    target_link_libraries(rufus_qt_widgets PUBLIC rufus_localization rufus_qt_write rufus_qt_iso_write rufus_qt_hash rufus_qt_save rufus_image_download)
    set_target_properties(rufus_qt_widgets PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    target_compile_features(rufus_qt_widgets PUBLIC cxx_std_17)
    target_include_directories(rufus_qt_widgets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(rufus_qt_widgets PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(rufus_qt_widgets PUBLIC Qt6::Widgets rufus_qt_analysis rufus_dos_locale rufus_policy_updates rufus_application_updates rufus_freedos_copy rufus_msdos_selection)
    target_compile_options(rufus_qt_widgets PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(RUFUS_BUILD_GUI)
    add_executable(rufus-linux qt/main.cpp)
    target_link_libraries(rufus-linux PRIVATE rufus_qt_widgets rufus_udisks)
    target_compile_definitions(rufus-linux PRIVATE RUFUS_PREPARE_HELPER_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-prepare-helper")
    target_compile_options(rufus-linux PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS rufus-linux RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES packaging/org.rufus.Rufus.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../res/icons/rufus-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME org.rufus.Rufus.png)
    endforeach()
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.txt" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/LICENSE.img2ffu"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/LICENSE.uefi-md5sum" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/NOTICE.uefi-md5sum"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rufus-linux-preview")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../docs/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rufus-linux-preview/docs"
        FILES_MATCHING PATTERN "*.md" PATTERN "linux-parity-register.json")
    configure_file(packaging/RELEASE.md.in RELEASE.md @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RELEASE.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rufus-linux-preview" RENAME README.md)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rufus-linux-preview/linux")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/release-evidence.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rufus-linux-preview/linux/packaging")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/syslinux/README.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rufus-linux-preview/linux/syslinux")
    configure_file(packaging/PKGBUILD.in PKGBUILD @ONLY)
    set(CPACK_PACKAGE_NAME "rufus-linux-preview")
    set(CPACK_PACKAGE_VENDOR "Rufus Linux preview contributors")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Rufus Linux preview - bootable USB creator")
    set(CPACK_PACKAGE_VERSION "${RUFUS_LINUX_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${RUFUS_LINUX_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.txt")
    set(CPACK_GENERATOR TGZ)
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
    include(packaging/DistributionPackages.cmake)
    include(CPack)
endif()

if(BUILD_TESTING)
    # Compile the original FreeDOS FAT routines against a test-only stdio
    # adapter. The Windows ms-sys/file.c and device paths are not linked.
    add_executable(test_freedos_boot_records tests/test_freedos_boot_records.c
        ../src/ms-sys/fat16.c ../src/ms-sys/fat32.c ../src/ms-sys/partition_info.c)
    target_compile_features(test_freedos_boot_records PRIVATE c_std_11)
    target_compile_definitions(test_freedos_boot_records PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_freedos_boot_records PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(test_freedos_boot_records PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/ms-sys/inc")
    add_test(NAME rufus_freedos_boot_records COMMAND test_freedos_boot_records)
    set_tests_properties(rufus_freedos_boot_records PROPERTIES LABELS "component;no-hardware" TIMEOUT 10)
    add_executable(dos_locale_probe tests/dos_locale_probe.c)
    target_compile_features(dos_locale_probe PRIVATE c_std_11)
    target_compile_options(dos_locale_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(dos_locale_probe PRIVATE rufus_dos_locale)
    add_executable(dos_locale_reference tests/dos_locale_reference.c)
    target_compile_features(dos_locale_reference PRIVATE c_std_11)
    # Keep the frozen source's release behavior for its missing-label assertions.
    # Only compare defined output; new error/fallback cases have separate checks.
    target_compile_definitions(dos_locale_reference PRIVATE NDEBUG)
    add_executable(freedos_copy_probe tests/freedos_copy_probe.cpp)
    target_compile_definitions(freedos_copy_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(freedos_copy_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(freedos_copy_probe PRIVATE rufus_freedos_copy)
    target_link_options(freedos_copy_probe PRIVATE -Wl,--wrap=fstatfs64 -Wl,--wrap=openat64 -Wl,--wrap=write
        -Wl,--wrap=pread64 -Wl,--wrap=ioctl -Wl,--wrap=fsync -Wl,--wrap=close
        -Wl,--wrap=open_memstream -Wl,--wrap=fclose -Wl,--wrap=fstatat64)
    add_executable(raw_write_probe tests/raw_write_probe.c)
    target_compile_definitions(raw_write_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(raw_write_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(raw_write_probe PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64
        -Wl,--wrap=pwrite -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    target_link_libraries(raw_write_probe PRIVATE rufus_uefi_ntfs)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(RUFUS_OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
    add_executable(download_signature_probe tests/download_signature_probe.c)
    target_compile_features(download_signature_probe PRIVATE c_std_11)
    target_compile_options(download_signature_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(download_signature_probe PRIVATE rufus_download_signature)
    target_link_options(download_signature_probe PRIVATE -Wl,--wrap=BN_bin2bn -Wl,--wrap=OSSL_PARAM_BLD_new
        -Wl,--wrap=OSSL_PARAM_BLD_to_param -Wl,--wrap=EVP_PKEY_fromdata -Wl,--wrap=EVP_MD_CTX_new
        -Wl,--wrap=EVP_DigestVerifyInit -Wl,--wrap=EVP_PKEY_CTX_set_rsa_padding -Wl,--wrap=EVP_DigestVerify)
    add_test(NAME rufus_download_signature COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_download_signature.py"
        --probe "$<TARGET_FILE:download_signature_probe>" --openssl "${RUFUS_OPENSSL_EXECUTABLE}"
        --key "${CMAKE_CURRENT_SOURCE_DIR}/../src/download_key.h"
        --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub-2.12")
    set_tests_properties(rufus_download_signature PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 30)
    add_executable(test_legacy_boot tests/test_legacy_boot.cpp tests/http_fixture.h)
    set_target_properties(test_legacy_boot PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_legacy_boot PRIVATE RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}")
    target_compile_definitions(test_legacy_boot PRIVATE RUFUS_RESOURCE_MKISOFS="${RUFUS_MKISOFS}")
    target_link_libraries(test_legacy_boot PRIVATE rufus_msdos_selection rufus_format_options rufus_fat_boot rufus_syslinux_native rufus_iso_extract rufus_qt_reporting Qt6::Test)
    add_test(NAME rufus_legacy_boot COMMAND test_legacy_boot)
    set_tests_properties(rufus_legacy_boot PROPERTIES LABELS "functional;no-hardware;network;legacy")
    add_executable(test_policy_updates tests/test_policy_updates.cpp tests/http_fixture.h)
    set_target_properties(test_policy_updates PROPERTIES AUTOMOC ON)
    target_link_libraries(test_policy_updates PRIVATE rufus_policy_updates rufus_qt_reporting Qt6::Test)
    add_test(NAME rufus_policy_updates COMMAND test_policy_updates)
    add_executable(test_signed_download tests/test_signed_download.cpp tests/http_fixture.h)
    set_target_properties(test_signed_download PROPERTIES AUTOMOC ON)
    target_compile_options(test_signed_download PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_signed_download PRIVATE
        RUFUS_SIGNED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub-2.12"
        RUFUS_OPENSSL="${RUFUS_OPENSSL_EXECUTABLE}")
    target_link_libraries(test_signed_download PRIVATE rufus_signed_download Qt6::Test)
    target_link_options(test_signed_download PRIVATE -Wl,--wrap=VerifyRufusDownloadSignature)
    add_test(NAME rufus_signed_download COMMAND test_signed_download)
    set_tests_properties(rufus_signed_download PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 30)
    add_test(NAME rufus_dos_locale COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dos_locale.py" "$<TARGET_FILE:dos_locale_probe>"
        "$<TARGET_FILE:dos_locale_reference>")
    set_tests_properties(rufus_dos_locale PROPERTIES LABELS "component;no-hardware" TIMEOUT 60)
    add_test(NAME rufus_grub4dos_copy COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_grub4dos_copy.py" "$<TARGET_FILE:freedos_copy_probe>")
    set_tests_properties(rufus_grub4dos_copy PROPERTIES LABELS "functional;no-hardware" TIMEOUT 30)
    add_test(NAME rufus_freedos_copy COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_copy.py" "$<TARGET_FILE:freedos_copy_probe>")
    set_tests_properties(rufus_freedos_copy PROPERTIES LABELS "functional;no-hardware" TIMEOUT 30)
    add_test(NAME rufus_freedos_startup COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_startup.py" "$<TARGET_FILE:freedos_copy_probe>"
        "$<TARGET_FILE:dos_locale_reference>")
    set_tests_properties(rufus_freedos_startup PROPERTIES LABELS "functional;no-hardware" TIMEOUT 30)
    add_test(NAME rufus_vm_supervision COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vm_supervision.py")
    set_tests_properties(rufus_vm_supervision PROPERTIES TIMEOUT 45 LABELS "functional;no-hardware")
    add_executable(test_secure_boot tests/test_secure_boot.cpp)
    set_target_properties(test_secure_boot PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_secure_boot PRIVATE RUFUS_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(test_secure_boot PRIVATE rufus_secure_boot rufus_qt_reporting Qt6::Test)
    target_link_options(test_secure_boot PRIVATE -Wl,--wrap=IsIsoScanCancelled)
    add_test(NAME rufus_secure_boot COMMAND test_secure_boot)
    add_executable(test_media_integrity tests/test_media_integrity.cpp)
    set_target_properties(test_media_integrity PROPERTIES AUTOMOC ON)
    target_compile_options(test_media_integrity PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_media_integrity PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_TEST_MD5_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/../res/md5" RUFUS_TEST_MKISOFS="${RUFUS_MKISOFS}")
    target_link_libraries(test_media_integrity PRIVATE rufus_media_integrity rufus_iso_extract rufus_qt_reporting rufus_format_policy rufus_prepare_process Qt6::Test)
    target_link_options(test_media_integrity PRIVATE -Wl,--wrap=IsRufusJobCancelled -Wl,--wrap=fsync)
    add_test(NAME rufus_media_integrity COMMAND test_media_integrity)
    set_tests_properties(rufus_media_integrity PROPERTIES LABELS "component;write;no-hardware" TIMEOUT 60)
    add_executable(media_appearance_probe tests/media_appearance_probe.cpp)
    target_compile_definitions(media_appearance_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(media_appearance_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(media_appearance_probe PRIVATE rufus_media_appearance)
    target_link_options(media_appearance_probe PRIVATE -Wl,--wrap=fstatfs64 -Wl,--wrap=pwrite64 -Wl,--wrap=pread64
        -Wl,--wrap=fsync -Wl,--wrap=syncfs -Wl,--wrap=renameat -Wl,--wrap=openat64)
    add_test(NAME rufus_media_appearance COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_media_appearance.py"
        "$<TARGET_FILE:media_appearance_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/rufus.ico")
    set_tests_properties(rufus_media_appearance PROPERTIES LABELS "component;write;no-hardware" TIMEOUT 60)
    add_executable(badblocks_probe tests/badblocks_probe.c)
    target_link_libraries(badblocks_probe PRIVATE rufus_badblocks)
    target_link_options(badblocks_probe PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    add_test(NAME rufus_badblocks COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_badblocks.py"
        "$<TARGET_FILE:badblocks_probe>" "$<TARGET_FILE:compressed_source_probe>")
    set_tests_properties(rufus_badblocks PROPERTIES LABELS "component;write;no-hardware" TIMEOUT 180)
    add_executable(zero_write_probe tests/zero_write_probe.c)
    target_link_libraries(zero_write_probe PRIVATE rufus_raw_write)
    target_link_options(zero_write_probe PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    add_test(NAME rufus_zero_write COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_zero_write.py"
        "$<TARGET_FILE:zero_write_probe>")
    set_tests_properties(rufus_zero_write PROPERTIES LABELS "component;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_raw_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_raw_write.py" "$<TARGET_FILE:raw_write_probe>")
    set_tests_properties(rufus_raw_write PROPERTIES LABELS "functional;write;no-hardware" TIMEOUT 30)
    add_test(NAME rufus_uefi_ntfs COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_uefi_ntfs.py" "$<TARGET_FILE:raw_write_probe>"
        "$<TARGET_FILE:partition_image_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/uefi/uefi-ntfs.img")
    set_tests_properties(rufus_uefi_ntfs PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 60)
    add_executable(test_image_source tests/test_image_source.c)
    add_executable(test_compression_policy tests/test_compression_policy.c)
    target_include_directories(test_compression_policy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_features(test_compression_policy PRIVATE c_std_11)
    target_compile_options(test_compression_policy PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_compression_policy COMMAND test_compression_policy)
    set_tests_properties(rufus_compression_policy PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 10)
    add_executable(compressed_source_probe tests/compressed_source_probe.c)
    add_executable(ffu_catalog_probe tests/ffu_catalog_probe.c platform/ffu_catalog.c)
    target_include_directories(ffu_catalog_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(ffu_catalog_probe PRIVATE OpenSSL::Crypto)
    target_compile_options(ffu_catalog_probe PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(test_compressed_source tests/test_compressed_source.c)
    target_compile_features(test_compressed_source PRIVATE c_std_11)
    target_compile_definitions(test_compressed_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_compressed_source PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_compressed_source PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64
        -Wl,--wrap=calloc -Wl,--wrap=free)
    target_link_libraries(test_compressed_source PRIVATE rufus_compressed_source Threads::Threads)
    add_test(NAME rufus_compressed_source COMMAND test_compressed_source)
    set_tests_properties(rufus_compressed_source PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 20)
    target_compile_definitions(compressed_source_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_source_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(compressed_source_probe PRIVATE rufus_compressed_source rufus_hash_core)
    add_executable(test_save_source tests/test_save_source.cpp)
    target_compile_options(test_save_source PRIVATE -UNDEBUG)
    target_link_libraries(test_save_source PRIVATE rufus_qt_save)
    add_test(NAME rufus_save_source COMMAND test_save_source)
    set_tests_properties(rufus_save_source PROPERTIES LABELS "component;no-hardware" TIMEOUT 10)
    add_executable(image_save_probe tests/image_save_probe.cpp)
    target_compile_definitions(image_save_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(image_save_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(image_save_probe PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    target_link_libraries(image_save_probe PRIVATE rufus_iso_export)
    add_test(NAME rufus_image_save COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_image_save.py" "$<TARGET_FILE:image_save_probe>")
    set_tests_properties(rufus_image_save PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_iso_export COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_export.py"
        "$<TARGET_FILE:image_save_probe>" "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_iso_export PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 240)
    add_executable(compressed_zstd_fault_probe tests/compressed_zstd_fault_probe.c)
    target_compile_features(compressed_zstd_fault_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_zstd_fault_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_zstd_fault_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(compressed_zstd_fault_probe PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64)
    target_link_libraries(compressed_zstd_fault_probe PRIVATE rufus_compressed_source)
    add_test(NAME rufus_compressed_zstd COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_zstd.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_zstd_fault_probe>")
    set_tests_properties(rufus_compressed_zstd PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 120)
    add_executable(compressed_vtsi_fault_probe tests/compressed_zstd_fault_probe.c)
    target_compile_features(compressed_vtsi_fault_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_vtsi_fault_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_vtsi_fault_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(compressed_vtsi_fault_probe PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64)
    target_link_libraries(compressed_vtsi_fault_probe PRIVATE rufus_compressed_source)
    add_test(NAME rufus_compressed_vtsi COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_vtsi.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_vtsi_fault_probe>")
    set_tests_properties(rufus_compressed_vtsi PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_compressed_vhd COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_vhd.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_zstd_fault_probe>"
        "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_compressed_vhd PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_compressed_vhdx COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_vhdx.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_write_probe>"
        "$<TARGET_FILE:compressed_zstd_fault_probe>")
    set_tests_properties(rufus_compressed_vhdx PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 120)
    add_executable(virtual_disk_probe tests/virtual_disk_probe.c)
    target_compile_definitions(virtual_disk_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(virtual_disk_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(virtual_disk_probe PRIVATE rufus_compressed_write)
    add_test(NAME rufus_virtual_disk COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_virtual_disk.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:virtual_disk_probe>")
    set_tests_properties(rufus_virtual_disk PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_compressed_ffu COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_ffu.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_write_probe>"
        "$<TARGET_FILE:compressed_zstd_fault_probe>")
    set_tests_properties(rufus_compressed_ffu PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_ffu_geometry COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_geometry.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_write_probe>" "$<TARGET_FILE:image_save_probe>")
    set_tests_properties(rufus_ffu_geometry PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_ffu_partial COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_partial.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_ffu_partial PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_ffu_deployment COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_deployment.py" "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_ffu_deployment PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_executable(ffu_codec_probe tests/ffu_codec_probe.c)
    target_compile_options(ffu_codec_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ffu_codec_probe PRIVATE rufus_compressed_source rufus_wimlib)
    add_test(NAME rufus_ffu_variants COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_variants.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:virtual_disk_probe>"
        "$<TARGET_FILE:ffu_codec_probe>" "$<TARGET_FILE:image_save_probe>")
    set_tests_properties(rufus_ffu_variants PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_ffu_capture COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_capture.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_write_probe>" "$<TARGET_FILE:ffu_codec_probe>" "$<TARGET_FILE:image_save_probe>")
    set_tests_properties(rufus_ffu_capture PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_ffu_layouts COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_layouts.py" "$<TARGET_FILE:image_save_probe>"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_ffu_layouts PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_executable(zip_supplement_probe tests/zip_supplement_probe.cpp)
    target_compile_options(zip_supplement_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(zip_supplement_probe PRIVATE rufus_prepare_source)
    target_link_options(zip_supplement_probe PRIVATE -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=renameat2)
    add_test(NAME rufus_zip_supplement COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_zip_supplement.py" "$<TARGET_FILE:zip_supplement_probe>")
    set_tests_properties(rufus_zip_supplement PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_ffu_stores COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_stores.py"
        "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_ffu_stores PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_ffu_catalog COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_catalog.py"
        "$<TARGET_FILE:ffu_catalog_probe>" "$<TARGET_FILE:compressed_source_probe>" "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_ffu_catalog PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_full_format COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_full_format.py" "$<TARGET_FILE:fat_format_probe>"
        "$<TARGET_FILE:ntfs_format_probe>" "$<TARGET_FILE:exfat_format_probe>" "$<TARGET_FILE:ext_format_probe>"
        "$<TARGET_FILE:zero_write_probe>")
    set_tests_properties(rufus_full_format PROPERTIES LABELS "component;format;write;no-hardware" TIMEOUT 180)

    add_test(NAME rufus_compressed_source_vectors COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_source.py" "$<TARGET_FILE:compressed_source_probe>")
    set_tests_properties(rufus_compressed_source_vectors PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 120)
    add_executable(compressed_xz_legacy_probe tests/compressed_source_probe.c
        platform/compressed_source.c platform/compressed_xz.c platform/compressed_bzip2.c platform/compressed_lzma.c
        platform/compressed_lzw.c platform/compressed_zip.c platform/compressed_zstd.c)
    target_sources(compressed_xz_legacy_probe PRIVATE platform/compressed_vtsi.c platform/compressed_vhd.c platform/compressed_vhdx.c platform/compressed_ffu.c platform/ffu_catalog.c platform/ffu_manifest.c platform/ffu_codec.c platform/virtual_disk.c platform/vhdx_replay.c)
    target_compile_features(compressed_xz_legacy_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_xz_legacy_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 RUFUS_XZ_LEGACY_CHECKS)
    target_compile_options(compressed_xz_legacy_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(compressed_xz_legacy_probe PRIVATE rufus_image_source rufus_hash_core ZLIB::ZLIB rufus_bled_zstd OpenSSL::Crypto)
    add_test(NAME rufus_compressed_zip COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_zip.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:test_compressed_source>")
    set_tests_properties(rufus_compressed_zip PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_compressed_zip_large COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_zip_large.py" "$<TARGET_FILE:compressed_source_probe>")
    set_tests_properties(rufus_compressed_zip_large PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 240)
    add_test(NAME rufus_compressed_xz COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_xz.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_xz_legacy_probe>" "$<TARGET_FILE:test_compressed_source>")
    set_tests_properties(rufus_compressed_xz PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 180)
    add_executable(compressed_bzip2_legacy_probe tests/compressed_bzip2_legacy_probe.c)
    target_compile_features(compressed_bzip2_legacy_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_bzip2_legacy_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_bzip2_legacy_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(compressed_bzip2_legacy_probe PRIVATE rufus_hash_core)
    add_test(NAME rufus_compressed_bzip2 COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_bzip2.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_bzip2_legacy_probe>" "$<TARGET_FILE:test_compressed_source>")
    set_tests_properties(rufus_compressed_bzip2 PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_compressed_bzip2_large COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_bzip2_large.py" "$<TARGET_FILE:compressed_source_probe>")
    set_tests_properties(rufus_compressed_bzip2_large PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 600)
    add_executable(compressed_lzma_legacy_probe tests/compressed_lzma_legacy_probe.c)
    target_compile_features(compressed_lzma_legacy_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_lzma_legacy_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_lzma_legacy_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(compressed_lzma_legacy_probe PRIVATE rufus_hash_core)
    add_test(NAME rufus_compressed_lzma COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_lzma.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_lzma_legacy_probe>" "$<TARGET_FILE:test_compressed_source>")
    set_tests_properties(rufus_compressed_lzma PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_compressed_lzma_large COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_lzma_large.py" "$<TARGET_FILE:compressed_source_probe>")
    set_tests_properties(rufus_compressed_lzma_large PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 660)
    add_executable(compressed_lzw_legacy_probe tests/compressed_lzw_legacy_probe.c)
    target_compile_features(compressed_lzw_legacy_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_lzw_legacy_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_lzw_legacy_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(compressed_lzw_legacy_probe PRIVATE rufus_hash_core)
    add_test(NAME rufus_compressed_lzw COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_lzw.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_lzw_legacy_probe>" "$<TARGET_FILE:test_compressed_source>")
    set_tests_properties(rufus_compressed_lzw PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_compressed_lzw_gzip_source COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_lzw.py" "$<TARGET_FILE:compressed_source_probe>"
        "$<TARGET_FILE:compressed_lzw_legacy_probe>" "$<TARGET_FILE:test_compressed_source>" --gzip-fallback)
    set_tests_properties(rufus_compressed_lzw_gzip_source PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 180)
    add_test(NAME rufus_compressed_lzw_large COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_lzw_large.py" "$<TARGET_FILE:compressed_source_probe>")
    set_tests_properties(rufus_compressed_lzw_large PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 660)
    add_executable(compressed_write_probe tests/compressed_write_probe.c)
    target_compile_features(compressed_write_probe PRIVATE c_std_11)
    target_compile_definitions(compressed_write_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(compressed_write_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(compressed_write_probe PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64
        -Wl,--wrap=pwrite -Wl,--wrap=pwrite64 -Wl,--wrap=fsync -Wl,--wrap=calloc -Wl,--wrap=free)
    target_link_libraries(compressed_write_probe PRIVATE rufus_compressed_write rufus_image_hash)
    add_test(NAME rufus_compressed_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_compressed_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_bzip2_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>" --bzip2)
    set_tests_properties(rufus_compressed_bzip2_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_xz_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>" --xz)
    set_tests_properties(rufus_compressed_xz_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_lzma_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>" --lzma)
    set_tests_properties(rufus_compressed_lzma_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_lzw_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>" --lzw)
    set_tests_properties(rufus_compressed_lzw_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_lzw_gzip_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>" --lzw-gzip)
    set_tests_properties(rufus_compressed_lzw_gzip_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_zip_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_zip_write.py" "$<TARGET_FILE:compressed_write_probe>")
    add_test(NAME rufus_compressed_zstd_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_write.py" "$<TARGET_FILE:compressed_write_probe>" --zstd)
    set_tests_properties(rufus_compressed_zstd_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_compressed_vtsi_write COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_vtsi_write.py" "$<TARGET_FILE:compressed_write_probe>")
    set_tests_properties(rufus_compressed_vtsi_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    set_tests_properties(rufus_compressed_zip_write PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    target_compile_definitions(test_image_source PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_image_source PRIVATE -Wall -Wextra)
    target_link_options(test_image_source PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64)
    target_link_libraries(test_image_source PRIVATE rufus_image_source)
    add_test(NAME rufus_image_source COMMAND test_image_source)
    set_tests_properties(rufus_image_source PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 10)
    add_executable(test_image_hash tests/test_image_hash.c)
    target_compile_definitions(test_image_hash PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_image_hash PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_image_hash PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64)
    target_link_libraries(test_image_hash PRIVATE rufus_image_hash)
    add_test(NAME rufus_image_hash COMMAND test_image_hash)
    set_tests_properties(rufus_image_hash PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 20)
    add_executable(test_image_hash_pipeline tests/test_image_hash_pipeline.c)
    target_compile_definitions(test_image_hash_pipeline PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_image_hash_pipeline PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_image_hash_pipeline PRIVATE -Wl,--wrap=pread -Wl,--wrap=pread64
        -Wl,--wrap=posix_memalign -Wl,--wrap=free -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join
        -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_mutex_destroy
        -Wl,--wrap=pthread_condattr_init -Wl,--wrap=pthread_condattr_setclock -Wl,--wrap=pthread_condattr_destroy
        -Wl,--wrap=pthread_cond_init -Wl,--wrap=pthread_cond_destroy)
    target_link_libraries(test_image_hash_pipeline PRIVATE rufus_image_hash)
    add_test(NAME rufus_image_hash_pipeline COMMAND test_image_hash_pipeline)
    set_tests_properties(rufus_image_hash_pipeline PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 20)
    add_executable(image_hash_probe tests/image_hash_probe.c)
    target_compile_definitions(image_hash_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(image_hash_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(image_hash_probe PRIVATE rufus_image_hash)
    add_test(NAME rufus_image_hash_vectors COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_image_hash.py" "$<TARGET_FILE:image_hash_probe>")
    set_tests_properties(rufus_image_hash_vectors PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 60)
    add_executable(iso_file_probe tests/iso_file_probe.c)
    target_compile_options(iso_file_probe PRIVATE -Wall -Wextra)
    target_link_options(iso_file_probe PRIVATE -Wl,--wrap=iso9660_iso_seek_read -Wl,--wrap=udf_read_block)
    target_link_libraries(iso_file_probe PRIVATE rufus_iso_scan)
    add_executable(wim_fixture tests/wim_fixture.c)
    target_compile_options(wim_fixture PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(wim_fixture PRIVATE rufus_wim_metadata)
    include(wimlib/tests.cmake)
    add_library(rufus_windows_to_go_fixture STATIC tests/windows_to_go_fixture.cpp tests/windows_to_go_fixture.h)
    target_compile_features(rufus_windows_to_go_fixture PRIVATE cxx_std_17)
    target_compile_definitions(rufus_windows_to_go_fixture PRIVATE
        RUFUS_MINIMAL_HIVE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hivex/minimal")
    target_link_libraries(rufus_windows_to_go_fixture PUBLIC rufus_wimlib Qt6::Core PRIVATE PkgConfig::RUFUS_HIVEX)
    add_executable(test_windows_to_go tests/test_windows_to_go.cpp)
    set_target_properties(test_windows_to_go PROPERTIES AUTOMOC ON)
    target_compile_features(test_windows_to_go PRIVATE cxx_std_17)
    target_compile_definitions(test_windows_to_go PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MINIMAL_HIVE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hivex/minimal"
        RUFUS_MKISOFS_PATH="${RUFUS_MKISOFS}" RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_compile_options(test_windows_to_go PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_windows_to_go PRIVATE rufus_windows_to_go rufus_windows_to_go_fixture PkgConfig::RUFUS_HIVEX Qt6::Test)
    add_test(NAME rufus_windows_to_go COMMAND test_windows_to_go)
    set_tests_properties(rufus_windows_to_go PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 60)
    add_executable(test_windows_bcd tests/test_windows_bcd.cpp)
    set_target_properties(test_windows_bcd PROPERTIES AUTOMOC ON)
    target_compile_features(test_windows_bcd PRIVATE cxx_std_17)
    target_compile_definitions(test_windows_bcd PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MINIMAL_HIVE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hivex/minimal")
    target_compile_options(test_windows_bcd PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_windows_bcd PRIVATE rufus_windows_bcd rufus_windows_customization rufus_wimlib PkgConfig::RUFUS_HIVEX Qt6::Test)
    target_link_options(test_windows_bcd PRIVATE -Wl,--wrap=hivex_commit -Wl,--wrap=hivex_close -Wl,--wrap=fsync)
    add_test(NAME rufus_windows_bcd COMMAND test_windows_bcd)
    set_tests_properties(rufus_windows_bcd PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 30)
    add_executable(test_windows_registry tests/test_windows_registry.cpp tests/registry_readback.c)
    set_target_properties(test_windows_registry PROPERTIES AUTOMOC ON)
    target_compile_features(test_windows_registry PRIVATE cxx_std_17)
    target_compile_options(test_windows_registry PRIVATE -Wall -Wextra)
    target_compile_definitions(test_windows_registry PRIVATE
        RUFUS_MINIMAL_HIVE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hivex/minimal")
    target_link_libraries(test_windows_registry PRIVATE rufus_windows_registry rufus_wimlib Qt6::Test)
    target_link_options(test_windows_registry PRIVATE -Wl,--wrap=hivex_node_get_child
        -Wl,--wrap=hivex_node_set_value -Wl,--wrap=hivex_commit -Wl,--wrap=hivex_close
        -Wl,--wrap=fsync -Wl,--wrap=close)
    add_test(NAME rufus_windows_registry COMMAND test_windows_registry)
    set_tests_properties(rufus_windows_registry PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 30)
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
    add_subdirectory(../tests/config_text config-text)
    add_subdirectory(../tests/winpe_setup winpe-setup)
    add_subdirectory(../tests/format_policy format-policy)
    add_subdirectory(../tests/progress_policy progress-policy)
    add_subdirectory(tests/hdd_score_reference)
    add_executable(device_operations_probe tests/device_operations_probe.cpp)
    target_compile_definitions(device_operations_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(device_operations_probe PRIVATE rufus_prepare_device rufus_qt_reporting)
    add_test(NAME rufus_device_source_batch COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_device_source_batch.py"
        "$<TARGET_FILE:device_operations_probe>" "$<TARGET_FILE:image_save_probe>")
    set_tests_properties(rufus_device_source_batch PROPERTIES LABELS "component;no-hardware" TIMEOUT 120)
    add_executable(test_native_disk_facts tests/test_native_disk_facts.cpp)
    set_target_properties(test_native_disk_facts PROPERTIES AUTOMOC ON)
    target_link_libraries(test_native_disk_facts PRIVATE rufus_native_disk_facts Qt6::Test)
    target_compile_options(test_native_disk_facts PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_native_disk_facts COMMAND test_native_disk_facts)
    set_tests_properties(rufus_native_disk_facts PROPERTIES LABELS "component;no-hardware" TIMEOUT 10)
    add_subdirectory(../tests/unattend unattend)
    add_executable(test_format_options tests/test_format_options.cpp)
    set_target_properties(test_format_options PROPERTIES AUTOMOC ON)
    target_link_libraries(test_format_options PRIVATE rufus_format_options
        rufus_partition_image rufus_fat_format rufus_qt_reporting Qt6::Test)
    target_include_directories(test_format_options PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/syslinux/libfat")
    target_compile_options(test_format_options PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_format_options COMMAND test_format_options)
    set_tests_properties(rufus_format_options PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 10)
    add_executable(test_persistence tests/test_persistence.cpp)
    set_target_properties(test_persistence PROPERTIES AUTOMOC ON)
    target_link_libraries(test_persistence PRIVATE rufus_format_options rufus_prepare_process
        rufus_partition_image rufus_qt_reporting Qt6::Test)
    target_compile_options(test_persistence PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_persistence COMMAND test_persistence)
    set_tests_properties(rufus_persistence PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 30)
    add_executable(test_config_patch tests/test_config_patch.cpp)
    add_executable(config_patch_probe tests/config_patch_probe.c)
    target_compile_definitions(config_patch_probe PRIVATE _GNU_SOURCE)
    target_compile_options(config_patch_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(config_patch_probe PRIVATE rufus_config_patch)
    set_target_properties(test_config_patch PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_config_patch PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_config_patch PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_config_patch PRIVATE rufus_config_patch Qt6::Test)
    target_link_options(test_config_patch PRIVATE -Wl,--wrap=write -Wl,--wrap=pread64
        -Wl,--wrap=pread -Wl,--wrap=fsync -Wl,--wrap=renameat -Wl,--wrap=close -Wl,--wrap=unlinkat
        -Wl,--wrap=fstat -Wl,--wrap=fstat64 -Wl,--wrap=fchown -Wl,--wrap=fchmod)
    add_test(NAME rufus_config_patch COMMAND test_config_patch)
    set_tests_properties(rufus_config_patch PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 30)
    add_executable(test_winpe_prepare tests/test_winpe_prepare.cpp)
    add_executable(winpe_prepare_probe tests/winpe_prepare_probe.cpp)
    target_compile_definitions(winpe_prepare_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(winpe_prepare_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(winpe_prepare_probe PRIVATE rufus_winpe_prepare)
    set(RUFUS_NTFS_FIXTURE_ROOT "" CACHE PATH "Optional retained NTFS-3G prefix for mount-free test fixtures only")
    if(RUFUS_NTFS_FIXTURE_ROOT)
        find_path(RUFUS_NTFS_FIXTURE_INCLUDE ntfs-3g/volume.h
            PATHS "${RUFUS_NTFS_FIXTURE_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
        find_library(RUFUS_NTFS_FIXTURE_LIBRARY ntfs-3g
            PATHS "${RUFUS_NTFS_FIXTURE_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        add_executable(ntfs_file_fixture tests/ntfs_file_fixture.c)
        target_compile_definitions(ntfs_file_fixture PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            HAVE_SYS_TYPES_H=1 HAVE_TIME_H=1)
        target_compile_options(ntfs_file_fixture PRIVATE -Wall -Wextra -Wpedantic)
        target_include_directories(ntfs_file_fixture PRIVATE "${RUFUS_NTFS_FIXTURE_INCLUDE}")
        target_link_libraries(ntfs_file_fixture PRIVATE "${RUFUS_NTFS_FIXTURE_LIBRARY}")
    endif()
    set_target_properties(test_winpe_prepare PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_winpe_prepare PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(test_winpe_prepare PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_winpe_prepare PRIVATE rufus_winpe_prepare Qt6::Test)
    target_link_options(test_winpe_prepare PRIVATE -Wl,--wrap=write -Wl,--wrap=pread64
        -Wl,--wrap=pread -Wl,--wrap=fsync -Wl,--wrap=renameat2 -Wl,--wrap=renameat
        -Wl,--wrap=fchmod -Wl,--wrap=linkat -Wl,--wrap=close)
    add_test(NAME rufus_winpe_prepare COMMAND test_winpe_prepare)
    set_tests_properties(rufus_winpe_prepare PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 30)
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
        -Wl,--wrap=ReadIsoFileRange -Wl,--wrap=AnalyzeIsoImageWithSource
        -Wl,--wrap=GetIsoSyslinuxConfig -Wl,--wrap=renameat2
        -Wl,--wrap=GetEfiSyslinuxConfigName -Wl,--wrap=pread -Wl,--wrap=pread64
        -Wl,--wrap=linkat -Wl,--wrap=unlinkat)
    add_test(NAME rufus_iso_extract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_extract.py" "$<TARGET_FILE:iso_extract_probe>"
        --mkisofs "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_iso_extract PROPERTIES
        LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    find_program(RUFUS_MKFS_FAT mkfs.fat REQUIRED)
    find_program(RUFUS_FSCK_FAT fsck.fat REQUIRED)
    find_program(RUFUS_MDIR mdir REQUIRED)
    find_program(RUFUS_SFDISK sfdisk REQUIRED)
    find_program(RUFUS_NTFSINFO ntfsinfo REQUIRED)
    find_program(RUFUS_NTFSLABEL ntfslabel REQUIRED)
    find_program(RUFUS_NTFSCK ntfsck)
    add_executable(exfat_format_probe tests/exfat_format_probe.cpp)
    target_compile_definitions(exfat_format_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(exfat_format_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(exfat_format_probe PRIVATE rufus_exfat_format PkgConfig::RUFUS_BLKID)
    target_link_options(exfat_format_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=clock_gettime
        -Wl,--wrap=RunFormatCommand -Wl,--wrap=pread64 -Wl,--wrap=blkid_get_library_version)
    find_program(RUFUS_FSCK_EXFAT NAMES fsck.exfat)
    find_program(RUFUS_DUMP_EXFAT NAMES dump.exfat)
    if(RUFUS_MKFS_EXFAT AND RUFUS_FSCK_EXFAT AND RUFUS_DUMP_EXFAT)
        add_test(NAME rufus_exfat_format COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_exfat_format.py" "$<TARGET_FILE:exfat_format_probe>"
            --mkfs "${RUFUS_MKFS_EXFAT}" --fsck "${RUFUS_FSCK_EXFAT}" --dump "${RUFUS_DUMP_EXFAT}")
        set_tests_properties(rufus_exfat_format PROPERTIES LABELS "functional;image;write;no-hardware"
            TIMEOUT 90 SKIP_RETURN_CODE 77)
    endif()
    add_executable(ntfs_format_probe tests/ntfs_format_probe.cpp)
    add_executable(ffu_finalize_probe tests/ffu_finalize_probe.cpp)
    target_link_libraries(ffu_finalize_probe PRIVATE rufus_raw_device rufus_qt_reporting)
    target_compile_options(ffu_finalize_probe PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_ffu_optimized COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ffu_optimized.py"
        "$<TARGET_FILE:ffu_finalize_probe>" "$<TARGET_FILE:compressed_write_probe>"
        "${RUFUS_MKNTFS}" "${RUFUS_NTFSRESIZE}")
    set_tests_properties(rufus_ffu_optimized PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 180)
    target_compile_definitions(ntfs_format_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(ntfs_format_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ntfs_format_probe PRIVATE rufus_ntfs_format)
    target_link_options(ntfs_format_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=clock_gettime)
    add_test(NAME rufus_ntfs_format COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ntfs_format.py" "$<TARGET_FILE:ntfs_format_probe>"
        --mkfs "${RUFUS_MKNTFS}" --info "${RUFUS_NTFSINFO}" --label "${RUFUS_NTFSLABEL}" --fsck "${RUFUS_NTFSCK}")
    set_tests_properties(rufus_ntfs_format PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    find_program(RUFUS_E2FSCK e2fsck REQUIRED)
    find_program(RUFUS_DUMPE2FS dumpe2fs REQUIRED)
    add_executable(ext_format_probe tests/ext_format_probe.cpp)
    target_compile_definitions(ext_format_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(ext_format_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ext_format_probe PRIVATE rufus_ext_format)
    target_link_options(ext_format_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=RunFormatCommand -Wl,--wrap=pread64)
    add_test(NAME rufus_ext_format COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_format.py" "$<TARGET_FILE:ext_format_probe>"
        --mkfs "${RUFUS_MKE2FS}" --fsck "${RUFUS_E2FSCK}" --dump "${RUFUS_DUMPE2FS}")
    set_tests_properties(rufus_ext_format PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    add_executable(ntfs_boot_probe tests/ntfs_boot_probe.cpp)
    target_compile_definitions(ntfs_boot_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(ntfs_boot_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ntfs_boot_probe PRIVATE rufus_ntfs_boot)
    target_link_options(ntfs_boot_probe PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=pwrite64
        -Wl,--wrap=fsync -Wl,--wrap=write_data)
    add_test(NAME rufus_ntfs_boot COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ntfs_boot.py"
        "$<TARGET_FILE:ntfs_boot_probe>" "$<TARGET_FILE:ntfs_format_probe>"
        --mkfs "${RUFUS_MKNTFS}" --info "${RUFUS_NTFSINFO}" --label "${RUFUS_NTFSLABEL}")
    set_tests_properties(rufus_ntfs_boot PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 120)
    add_executable(partition_image_probe tests/partition_image_probe.c)
    target_compile_definitions(partition_image_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(partition_image_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(partition_image_probe PRIVATE rufus_partition_image)
    target_link_options(partition_image_probe PRIVATE -Wl,--wrap=fsync
        -Wl,--wrap=fdisk_add_partition -Wl,--wrap=fdisk_write_disklabel
        -Wl,--wrap=fdisk_gpt_set_partition_attrs)
    add_test(NAME rufus_partition_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_partition_image.py"
        "$<TARGET_FILE:partition_image_probe>" "$<TARGET_FILE:fat_format_probe>"
        --uefi-ntfs "${CMAKE_CURRENT_SOURCE_DIR}/../res/uefi/uefi-ntfs.img"
        --sfdisk "${RUFUS_SFDISK}" --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_partition_image PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_executable(mbr_image_probe tests/mbr_image_probe.c)
    target_compile_definitions(mbr_image_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(mbr_image_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mbr_image_probe PRIVATE rufus_partition_image)
    target_link_options(mbr_image_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=pwrite64 -Wl,--wrap=pread64
        -Wl,--wrap=fdisk_add_partition -Wl,--wrap=fdisk_write_disklabel -Wl,--wrap=fdisk_toggle_partition_flag)
    add_test(NAME rufus_syslinux_mbr_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_image.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>" --sfdisk "${RUFUS_SFDISK}")
    set_tests_properties(rufus_syslinux_mbr_image PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_old_bios COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_old_bios.py" "$<TARGET_FILE:mbr_image_probe>")
    set_tests_properties(rufus_old_bios PROPERTIES LABELS "component;image;write;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_syslinux_mbr_prepare COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_prepare.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>")
    set_tests_properties(rufus_syslinux_mbr_prepare PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_syslinux_ntfs_mbr_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_image.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
        --sfdisk "${RUFUS_SFDISK}" --filesystem ntfs)
    set_tests_properties(rufus_syslinux_ntfs_mbr_image PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_syslinux_uefi_ntfs_mbr_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_image.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
        --sfdisk "${RUFUS_SFDISK}" --filesystem ntfs --boot-code syslinux --uefi-ntfs)
    set_tests_properties(rufus_syslinux_uefi_ntfs_mbr_image PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_grub2_uefi_ntfs_mbr_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_image.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
        --sfdisk "${RUFUS_SFDISK}" --filesystem ntfs --boot-code grub2 --uefi-ntfs)
    set_tests_properties(rufus_grub2_uefi_ntfs_mbr_image PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_fat16_mbr_image COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_image.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
        --sfdisk "${RUFUS_SFDISK}" --fat-bits 16)
    add_test(NAME rufus_fat16_mbr_prepare COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_prepare.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>" --fat-bits 16)
    set_tests_properties(rufus_fat16_mbr_image rufus_fat16_mbr_prepare
        PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    foreach(bootstrap IN ITEMS rufus win7)
        foreach(mode IN ITEMS bios dual)
            set(windows_mbr_args --boot-code "${bootstrap}" --filesystem ntfs)
            set(windows_mbr_name "rufus_windows_${bootstrap}_ntfs_mbr")
            if(mode STREQUAL "dual")
                list(APPEND windows_mbr_args --uefi-ntfs)
                set(windows_mbr_name "rufus_windows_${bootstrap}_uefi_ntfs_mbr")
            endif()
            foreach(stage IN ITEMS image prepare)
                add_test(NAME ${windows_mbr_name}_${stage} COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                    "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                    ${windows_mbr_args})
                set_tests_properties(${windows_mbr_name}_${stage}
                    PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
            endforeach()
        endforeach()
        foreach(bits IN ITEMS 16 32)
            foreach(stage IN ITEMS image prepare)
                add_test(NAME rufus_freedos_${bootstrap}_fat${bits}_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                    "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                    --boot-code "${bootstrap}" --fat-bits "${bits}")
                set_tests_properties(rufus_freedos_${bootstrap}_fat${bits}_mbr_${stage}
                    PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
            endforeach()
        endforeach()
    endforeach()
    foreach(bits IN ITEMS 16 32)
        foreach(stage IN ITEMS image prepare)
            add_test(NAME rufus_winpe_fat${bits}_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                --boot-code rufus-masquerade --fat-bits "${bits}")
            set_tests_properties(rufus_winpe_fat${bits}_mbr_${stage}
                PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
        endforeach()
        foreach(stage IN ITEMS image prepare)
            add_test(NAME rufus_grub2_fat${bits}_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                --boot-code grub2 --fat-bits "${bits}")
            set_tests_properties(rufus_grub2_fat${bits}_mbr_${stage}
                PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
        endforeach()
    endforeach()
    foreach(stage IN ITEMS image prepare)
        add_test(NAME rufus_winpe_ntfs_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
            "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
            --boot-code rufus-masquerade --filesystem ntfs)
        set_tests_properties(rufus_winpe_ntfs_mbr_${stage}
            PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    endforeach()
    foreach(stage IN ITEMS image prepare)
        add_test(NAME rufus_grub2_ntfs_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
            "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
            --boot-code grub2 --filesystem ntfs)
        set_tests_properties(rufus_grub2_ntfs_mbr_${stage}
            PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    endforeach()
    foreach(format IN ITEMS fat16 fat32 ntfs exfat)
        if(format STREQUAL "fat16")
            set(mbr_format_args --fat-bits 16)
        elseif(format STREQUAL "fat32")
            set(mbr_format_args --fat-bits 32)
        else()
            set(mbr_format_args --filesystem "${format}")
        endif()
        foreach(stage IN ITEMS image prepare)
            add_test(NAME rufus_nonboot_${format}_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                --non-bootable ${mbr_format_args})
            set_tests_properties(rufus_nonboot_${format}_mbr_${stage}
                PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
        endforeach()
    endforeach()
    foreach(format IN ITEMS ntfs exfat)
        foreach(stage IN ITEMS image prepare)
            add_test(NAME rufus_uefi_${format}_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                --uefi-ntfs --filesystem "${format}")
            set_tests_properties(rufus_uefi_${format}_mbr_${stage}
                PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
        endforeach()
    endforeach()
    add_test(NAME rufus_uefi_ntfs_mbr COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_uefi_ntfs.py" "$<TARGET_FILE:raw_write_probe>"
        "$<TARGET_FILE:partition_image_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/uefi/uefi-ntfs.img"
        "$<TARGET_FILE:mbr_image_probe>")
    set_tests_properties(rufus_uefi_ntfs_mbr PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 60)
    add_test(NAME rufus_sfd_prepare COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sfd_prepare.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>" "$<TARGET_FILE:fat_format_probe>"
        --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}" --sfdisk "${RUFUS_SFDISK}")
    set_tests_properties(rufus_sfd_prepare PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_executable(fat_format_probe tests/fat_format_probe.c)
    target_compile_definitions(fat_format_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(fat_format_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fat_format_probe PRIVATE rufus_fat_format)
    target_link_options(fat_format_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=clock_gettime)
    add_executable(freedos_boot_probe tests/freedos_boot_probe.c)
    target_compile_definitions(freedos_boot_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(freedos_boot_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(freedos_boot_probe PRIVATE rufus_fat_boot rufus_winpe_prepare)
    target_link_options(freedos_boot_probe PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=pwrite64
        -Wl,--wrap=fsync -Wl,--wrap=write_data)
    add_test(NAME rufus_freedos_boot COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_boot.py" "$<TARGET_FILE:freedos_boot_probe>"
        "$<TARGET_FILE:fat_format_probe>" --mkfs "${RUFUS_MKFS_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_freedos_boot PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_standard_fat_boot COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_boot.py" "$<TARGET_FILE:freedos_boot_probe>"
        "$<TARGET_FILE:fat_format_probe>" --mkfs "${RUFUS_MKFS_FAT}" --mdir "${RUFUS_MDIR}" --standard)
    set_tests_properties(rufus_standard_fat_boot PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_windows_fat_boot COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_boot.py" "$<TARGET_FILE:freedos_boot_probe>"
        "$<TARGET_FILE:fat_format_probe>" --mkfs "${RUFUS_MKFS_FAT}" --mdir "${RUFUS_MDIR}" --windows)
    set_tests_properties(rufus_windows_fat_boot PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_winpe_fat_boot COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_boot.py" "$<TARGET_FILE:freedos_boot_probe>"
        "$<TARGET_FILE:fat_format_probe>" --mkfs "${RUFUS_MKFS_FAT}" --mdir "${RUFUS_MDIR}" --winpe)
    set_tests_properties(rufus_winpe_fat_boot PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    foreach(layout IN ITEMS i386 minint)
        set(winpe_options --winpe-bootmgr)
        if(layout STREQUAL "minint")
            list(APPEND winpe_options --minint)
        endif()
        add_test(NAME rufus_winpe_bootmgr_fat_${layout} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_freedos_boot.py" "$<TARGET_FILE:freedos_boot_probe>"
            "$<TARGET_FILE:fat_format_probe>" --mkfs "${RUFUS_MKFS_FAT}" --mdir "${RUFUS_MDIR}" ${winpe_options})
        set_tests_properties(rufus_winpe_bootmgr_fat_${layout}
            PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    endforeach()
    add_executable(grub2_core_probe tests/grub2_core_probe.c)
    target_compile_definitions(grub2_core_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(grub2_core_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(grub2_core_probe PRIVATE rufus_partition_image)
    target_link_options(grub2_core_probe PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    add_test(NAME rufus_grub2_core COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_grub2_core.py" "$<TARGET_FILE:grub2_core_probe>"
        "$<TARGET_FILE:mbr_image_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/grub2/core.img")
    set_tests_properties(rufus_grub2_core PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_grub4dos_sbr COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_grub2_core.py" "$<TARGET_FILE:grub2_core_probe>"
        "$<TARGET_FILE:mbr_image_probe>" "${CMAKE_CURRENT_SOURCE_DIR}/../res/grub/grldr.mbr" --grub4dos)
    set_tests_properties(rufus_grub4dos_sbr PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    foreach(format IN ITEMS fat16 fat32 ntfs)
        if(format STREQUAL "ntfs")
            set(grub4dos_format_args --filesystem ntfs)
        elseif(format STREQUAL "fat16")
            set(grub4dos_format_args --fat-bits 16)
        else()
            set(grub4dos_format_args --fat-bits 32)
        endif()
        foreach(stage IN ITEMS image prepare)
            add_test(NAME rufus_grub4dos_${format}_mbr_${stage} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mbr_${stage}.py"
                "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:partition_image_probe>"
                --boot-code grub4dos ${grub4dos_format_args})
            set_tests_properties(rufus_grub4dos_${format}_mbr_${stage}
                PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
        endforeach()
    endforeach()
    add_test(NAME rufus_grub2_ext_layout COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_grub2_ext_layout.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:grub2_core_probe>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../res/grub2/core.img" --sfdisk "${RUFUS_SFDISK}")
    set_tests_properties(rufus_grub2_ext_layout PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_fat_format COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat_format.py" "$<TARGET_FILE:fat_format_probe>"
        --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_fat_format PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_fat16_format COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat16_format.py" "$<TARGET_FILE:fat_format_probe>"
        --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_fat16_format PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_fat16_partition COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat16_partition.py"
        "$<TARGET_FILE:partition_image_probe>" "$<TARGET_FILE:fat_format_probe>"
        --sfdisk "${RUFUS_SFDISK}" --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_fat16_partition PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_fat16_mbr_partition COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat16_partition.py"
        "$<TARGET_FILE:mbr_image_probe>" "$<TARGET_FILE:fat_format_probe>" --mbr
        --sfdisk "${RUFUS_SFDISK}" --mkfs "${RUFUS_MKFS_FAT}" --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_fat16_mbr_partition PROPERTIES LABELS "functional;image;write;no-hardware" TIMEOUT 90)
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
    add_executable(syslinux_source_probe probes/syslinux/source.cpp)
    add_executable(syslinux_install_probe probes/syslinux/install.cpp)
    foreach(suffix IN ITEMS "" "4")
        set(RUFUS_SYSLINUX${suffix}_TEST_INSTALLER "" CACHE FILEPATH
            "Optional normal-build Syslinux executable for instrumented adapter tests")
        set(syslinux${suffix}_test_installer "$<TARGET_FILE:rufus-syslinux${suffix}>")
        if(RUFUS_SYSLINUX${suffix}_TEST_INSTALLER)
            if(NOT EXISTS "${RUFUS_SYSLINUX${suffix}_TEST_INSTALLER}")
                message(FATAL_ERROR "Missing test installer: ${RUFUS_SYSLINUX${suffix}_TEST_INSTALLER}")
            endif()
            set(syslinux${suffix}_test_installer "${RUFUS_SYSLINUX${suffix}_TEST_INSTALLER}")
        endif()
    endforeach()
    if(TARGET rufus-syslinux4)
        find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
        # Reuse the existing file-only runner probe; no v4 application route yet.
        add_executable(syslinux_command_probe probes/syslinux/command.c platform/format_command.c)
        target_compile_features(syslinux_command_probe PRIVATE c_std_11)
        target_compile_definitions(syslinux_command_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
        target_compile_options(syslinux_command_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_include_directories(syslinux_command_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_options(syslinux_command_probe PRIVATE -Wl,--wrap=fsync -Wl,--wrap=clock_gettime)
        add_test(NAME rufus_syslinux4_command COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/probes/syslinux/test_command.py" "$<TARGET_FILE:syslinux_command_probe>"
            "$<TARGET_FILE:rufus-syslinux4>" "${RUFUS_SYSLINUX4_UPSTREAM}" --major 4)
        set_tests_properties(rufus_syslinux4_command PROPERTIES
            LABELS "functional;image;no-hardware" TIMEOUT 90)
        add_test(NAME rufus_syslinux4_package COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/syslinux/test_package.py"
            "${CMAKE_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}" "$<CONFIG>"
            "$<TARGET_FILE:rufus-syslinux4>" "$<TARGET_FILE:syslinux_install_probe>"
            "${RUFUS_SYSLINUX4_UPSTREAM}" "${CMAKE_INSTALL_LIBEXECDIR}" "${CMAKE_INSTALL_DATADIR}" --major 4)
        set_tests_properties(rufus_syslinux4_package PROPERTIES
            LABELS "functional;package;image;boot;no-hardware" TIMEOUT 120)
    endif()
    if(TARGET rufus-syslinux)
        find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
        add_executable(syslinux_selection_probe probes/syslinux/selection.cpp)
        target_compile_features(syslinux_selection_probe PRIVATE cxx_std_17)
        target_compile_options(syslinux_selection_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(syslinux_selection_probe PRIVATE rufus_syslinux_native)
        add_test(NAME rufus_syslinux_native_selection COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/probes/syslinux/test_selection.py"
            "$<TARGET_FILE:syslinux_selection_probe>" "${RUFUS_SYSLINUX_UPSTREAM}")
        set_tests_properties(rufus_syslinux_native_selection PROPERTIES
            LABELS "component;image;no-hardware" TIMEOUT 30)
        foreach(fat_bits 16 32)
            add_test(NAME rufus_syslinux_fat${fat_bits}_install COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/probes/syslinux/test_install.py"
                "$<TARGET_FILE:syslinux_install_probe>" "${syslinux_test_installer}"
                "${RUFUS_SYSLINUX_UPSTREAM}" "${CMAKE_CURRENT_BINARY_DIR}" --fat-bits ${fat_bits})
            set_tests_properties(rufus_syslinux_fat${fat_bits}_install PROPERTIES
                LABELS "functional;image;no-hardware" TIMEOUT 120)
        endforeach()
        add_test(NAME rufus_syslinux_package COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/syslinux/test_package.py"
            "${CMAKE_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}" "$<CONFIG>"
            "$<TARGET_FILE:rufus-syslinux>" "$<TARGET_FILE:syslinux_selection_probe>"
            "${RUFUS_SYSLINUX_UPSTREAM}" "${CMAKE_INSTALL_LIBEXECDIR}" "${CMAKE_INSTALL_DATADIR}"
            --extlinux "$<TARGET_FILE:rufus-extlinux>")
        set_tests_properties(rufus_syslinux_package PROPERTIES
            LABELS "functional;package;image;boot;no-hardware" TIMEOUT 120)
        add_test(NAME rufus_syslinux_standalone_probe COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/probes/syslinux/test_standalone.py"
            "${syslinux_test_installer}" "${RUFUS_SYSLINUX_UPSTREAM}" "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(rufus_syslinux_standalone_probe PROPERTIES
            LABELS "functional;image;boot;no-hardware" TIMEOUT 120)
    endif()
    foreach(major 4 6)
        set(suffix "")
        set(other_suffix "4")
        if(major EQUAL 4)
            set(suffix "4")
            set(other_suffix "")
        endif()
        if(NOT TARGET rufus-syslinux${suffix})
            continue()
        endif()
        set(other_installer_args)
        if(TARGET rufus-syslinux${other_suffix})
            set(other_installer_args --other-installer "${syslinux${other_suffix}_test_installer}")
        endif()
        foreach(fat_bits 16 32)
            add_test(NAME rufus_syslinux${suffix}_fat${fat_bits}_standalone_install COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/probes/syslinux/test_install.py"
                "$<TARGET_FILE:syslinux_install_probe>" "${syslinux${suffix}_test_installer}"
                "${RUFUS_SYSLINUX${suffix}_UPSTREAM}" "${CMAKE_CURRENT_BINARY_DIR}"
                --fat-bits ${fat_bits} --standalone --major ${major} ${other_installer_args})
            set_tests_properties(rufus_syslinux${suffix}_fat${fat_bits}_standalone_install PROPERTIES
                LABELS "functional;image;no-hardware" TIMEOUT 120)
        endforeach()
    endforeach()
    add_executable(syslinux_memory_probe probes/syslinux/test_patcher.c)
    add_executable(syslinux_ntfs_probe probes/syslinux/ntfs_install.cpp)
    target_compile_features(syslinux_ntfs_probe PRIVATE cxx_std_17)
    target_compile_definitions(syslinux_ntfs_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(syslinux_ntfs_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(syslinux_ntfs_probe PRIVATE rufus_syslinux_install)
    target_link_options(syslinux_ntfs_probe PRIVATE -Wl,--wrap=RunInstallerCommand
        -Wl,--wrap=CheckImageFileSource
        -Wl,--wrap=fsync -Wl,--wrap=syncfs -Wl,--wrap=pread64 -Wl,--wrap=ioctl -Wl,--wrap=fstatfs64
        -Wl,--wrap=posix_fadvise64 -Wl,--wrap=fgetxattr -Wl,--wrap=fsetxattr)
    target_compile_features(syslinux_memory_probe PRIVATE c_std_11)
    target_include_directories(syslinux_memory_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    add_test(NAME rufus_syslinux_packed_fields COMMAND syslinux_memory_probe)
    set_tests_properties(rufus_syslinux_packed_fields PROPERTIES LABELS "component;no-hardware")
    target_compile_features(syslinux_install_probe PRIVATE cxx_std_17)
    target_compile_options(syslinux_install_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(syslinux_install_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(syslinux_install_probe PRIVATE rufus_syslinux_install)
    target_link_options(syslinux_install_probe PRIVATE -Wl,--wrap=RunInstallerCommand -Wl,--wrap=fsync -Wl,--wrap=pread64)
    target_compile_features(syslinux_source_probe PRIVATE cxx_std_17)
    target_compile_options(syslinux_source_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(syslinux_source_probe PRIVATE rufus_syslinux_native rufus_image_source)
    target_link_options(syslinux_source_probe PRIVATE -Wl,--wrap=ReadIsoFileRange)
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
    add_executable(test_freedos_locale_dialog tests/test_freedos_locale_dialog.cpp)
    set_target_properties(test_freedos_locale_dialog PROPERTIES AUTOMOC ON)
    target_compile_options(test_freedos_locale_dialog PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_freedos_locale_dialog PRIVATE rufus_qt_widgets Qt6::Test)
    add_test(NAME rufus_freedos_locale_dialog COMMAND test_freedos_locale_dialog)
    set_tests_properties(rufus_freedos_locale_dialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "functional;widgets;no-hardware" TIMEOUT 30)
    add_executable(test_windows_install_options tests/test_windows_install_options.cpp)
    set_target_properties(test_windows_install_options PROPERTIES AUTOMOC ON)
    target_compile_options(test_windows_install_options PRIVATE -Wall -Wextra)
    target_compile_definitions(test_windows_install_options PRIVATE RUFUS_MKISOFS="${RUFUS_MKISOFS}")
    target_link_libraries(test_windows_install_options PRIVATE rufus_windows_customization rufus_wimlib Qt6::Test)
    target_link_options(test_windows_install_options PRIVATE -Wl,--wrap=ReadIsoFileRange
        -Wl,--wrap=wimlib_get_image_property)
    add_test(NAME rufus_windows_install_options COMMAND test_windows_install_options)
    set_tests_properties(rufus_windows_install_options PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 30)
    add_executable(test_windows_customization tests/test_windows_customization.cpp)
    set_target_properties(test_windows_customization PROPERTIES AUTOMOC ON)
    target_compile_options(test_windows_customization PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_windows_customization PRIVATE rufus_windows_customization Qt6::Test)
    target_link_options(test_windows_customization PRIVATE -Wl,--wrap=WriteUnattendXml -Wl,--wrap=fclose)
    add_test(NAME rufus_windows_customization COMMAND test_windows_customization)
    set_tests_properties(rufus_windows_customization PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 10)
    add_executable(test_windows_setup tests/test_windows_setup.cpp tests/registry_readback.c)
    set_target_properties(test_windows_setup PROPERTIES AUTOMOC ON)
    target_compile_options(test_windows_setup PRIVATE -Wall -Wextra)
    target_compile_definitions(test_windows_setup PRIVATE
        RUFUS_MINIMAL_HIVE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hivex/minimal"
        RUFUS_SETUP_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/../res/setup")
    target_link_libraries(test_windows_setup PRIVATE rufus_windows_customization rufus_windows_registry
        rufus_wim_read_file Qt6::Test)
    target_link_options(test_windows_setup PRIVATE -Wl,--wrap=pwrite -Wl,--wrap=pwrite64 -Wl,--wrap=hivex_commit -Wl,--wrap=wimlib_extract_paths
        -Wl,--wrap=wimlib_overwrite -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=WriteUnattendXml)
    add_test(NAME rufus_windows_setup COMMAND test_windows_setup)
    set_tests_properties(rufus_windows_setup PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 30)
    add_executable(test_iso_write_check tests/test_iso_write_check.cpp)
    set_target_properties(test_iso_write_check PROPERTIES AUTOMOC ON)
    target_compile_options(test_iso_write_check PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_iso_write_check PRIVATE RUFUS_MKISOFS="${RUFUS_MKISOFS}"
        RUFUS_WIM_FIXTURE="$<TARGET_FILE:wim_fixture>")
    add_dependencies(test_iso_write_check wim_fixture)
    target_link_libraries(test_iso_write_check PRIVATE rufus_iso_write_check Qt6::Test)
    target_link_options(test_iso_write_check PRIVATE -Wl,--wrap=AnalyzeIsoImageWithSource
        -Wl,--wrap=ReadIsoFileRange -Wl,--wrap=CheckIsoTree -Wl,--wrap=OpenWimFromIsoReader
        -Wl,--wrap=ReadWimFile)
    add_test(NAME rufus_iso_write_check COMMAND test_iso_write_check)
    set_tests_properties(rufus_iso_write_check PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 180)
    add_executable(iso_write_check_probe tests/iso_write_check_probe.cpp)
    target_compile_options(iso_write_check_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(iso_write_check_probe PRIVATE rufus_secure_boot rufus_iso_write_check)
    add_test(NAME rufus_iso_esp COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_esp.py" "$<TARGET_FILE:partition_image_probe>"
        "$<TARGET_FILE:iso_write_check_probe>")
    set_tests_properties(rufus_iso_esp PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 120)
    add_executable(test_prepare_source tests/test_prepare_source.cpp)
    set_target_properties(test_prepare_source PROPERTIES AUTOMOC ON)
    target_compile_options(test_prepare_source PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_prepare_source PRIVATE rufus_prepare_source Qt6::Test)
    add_test(NAME rufus_prepare_source COMMAND test_prepare_source)
    set_tests_properties(rufus_prepare_source PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 10)
    add_executable(test_prepare_grub2_core tests/test_prepare_grub2_core.cpp)
    set_target_properties(test_prepare_grub2_core PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_prepare_grub2_core PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_definitions(test_prepare_grub2_core PRIVATE
        RUFUS_PREPARE_PROBE="$<TARGET_FILE:prepare_helper_probe>")
    target_compile_options(test_prepare_grub2_core PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_prepare_grub2_core PRIVATE rufus_prepare_source Qt6::Test)
    target_link_options(test_prepare_grub2_core PRIVATE -Wl,--wrap=pread64)
    add_dependencies(test_prepare_grub2_core prepare_helper_probe)
    add_test(NAME rufus_prepare_grub2_core COMMAND test_prepare_grub2_core)
    set_tests_properties(rufus_prepare_grub2_core PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 10)
    add_executable(test_grub2_cache tests/test_grub2_cache.cpp)
    set_target_properties(test_grub2_cache PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_grub2_cache PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_GRUB2_CORE="${CMAKE_CURRENT_SOURCE_DIR}/../res/grub2/core.img"
        RUFUS_GRUB4DOS_MBR="${CMAKE_CURRENT_SOURCE_DIR}/../res/grub/grldr.mbr")
    target_compile_options(test_grub2_cache PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_grub2_cache PRIVATE rufus_prepare_source Qt6::Test)
    target_link_options(test_grub2_cache PRIVATE -Wl,--wrap=open64 -Wl,--wrap=pread64)
    add_test(NAME rufus_grub2_cache COMMAND test_grub2_cache)
    set_tests_properties(rufus_grub2_cache PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 10)
    add_executable(test_grub2_cache_store tests/test_grub2_cache_store.cpp)
    set_target_properties(test_grub2_cache_store PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_grub2_cache_store PRIVATE
        RUFUS_SIGNED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub-2.12")
    target_compile_options(test_grub2_cache_store PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_grub2_cache_store PRIVATE rufus_prepare_source Qt6::Test)
    target_link_options(test_grub2_cache_store PRIVATE
        -Wl,--wrap=_ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE
        -Wl,--wrap=_ZN9QIODevice5writeEPKcx -Wl,--wrap=_ZN9QSaveFile6commitEv)
    add_test(NAME rufus_grub2_cache_store COMMAND test_grub2_cache_store)
    set_tests_properties(rufus_grub2_cache_store PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 10)
    add_test(NAME rufus_iso_source_batch COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_source_batch.py"
        --write-check "$<TARGET_FILE:iso_write_check_probe>" --extract "$<TARGET_FILE:iso_extract_probe>" --mkisofs "${RUFUS_MKISOFS}")
    set_tests_properties(rufus_iso_source_batch PROPERTIES LABELS "component;image;no-hardware" TIMEOUT 120)
    add_executable(test_syslinux_resources tests/test_syslinux_resources.cpp tests/http_fixture.h)
    set_target_properties(test_syslinux_resources PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_syslinux_resources PRIVATE RUFUS_RESOURCE_MKISOFS="${RUFUS_MKISOFS}")
    target_link_libraries(test_syslinux_resources PRIVATE rufus_iso_extract  rufus_qt_analysis rufus_qt_iso_write Qt6::Test Qt6::Network)
    target_compile_definitions(test_syslinux_resources PRIVATE RUFUS_RESOURCE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download")
    add_test(NAME rufus_syslinux_resources COMMAND test_syslinux_resources)
    set_tests_properties(rufus_syslinux_resources PROPERTIES LABELS "component;no-hardware;download" TIMEOUT 90)
    add_executable(test_grub2_core_selection tests/test_grub2_core_selection.cpp)
    set_target_properties(test_grub2_core_selection PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_grub2_core_selection PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_SIGNED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub-2.12"
        RUFUS_GRUB4DOS_SIGNED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub4dos-0.4.6a")
    target_compile_options(test_grub2_core_selection PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_grub2_core_selection PRIVATE rufus_qt_analysis Qt6::Test)
    target_link_options(test_grub2_core_selection PRIVATE -Wl,--wrap=_ZN9QSaveFile6commitEv -Wl,--wrap=open64)
    add_test(NAME rufus_grub2_core_selection COMMAND test_grub2_core_selection)
    set_tests_properties(rufus_grub2_core_selection PROPERTIES LABELS "functional;image;no-hardware" TIMEOUT 30)
    add_executable(prepare_helper_probe helper/prepare_main.cpp helper/boot_resources.qrc tests/prepare_helper_faults.c)
    target_link_libraries(prepare_helper_probe PRIVATE rufus_format_options rufus_iso_write_check rufus_raw_device)
    set_target_properties(prepare_helper_probe PROPERTIES AUTOMOC ON AUTORCC ON)
    target_compile_definitions(prepare_helper_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
        RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
        RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_compile_options(prepare_helper_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(prepare_helper_probe PRIVATE -Wl,--wrap=posix_spawn -Wl,--wrap=fsync
        -Wl,--wrap=getuid -Wl,--wrap=geteuid
        -Wl,--wrap=_ZN5rufus19PrepareDeviceTarget4openEjPKNS_16PrepareIsoSourceE)
    target_link_libraries(prepare_helper_probe PRIVATE rufus_format_options rufus_prepare_process rufus_qt_reporting
        rufus_partition_image rufus_fat_format rufus_prepare_device rufus_prepare_source rufus_uefi_ntfs
        rufus_fat_boot rufus_freedos_copy)
    add_executable(test_prepare_image_process tests/test_prepare_image_process.cpp)
    target_include_directories(test_prepare_image_process PRIVATE ../src)
    set_target_properties(test_prepare_image_process PROPERTIES AUTOMOC ON)
    target_compile_options(test_prepare_image_process PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_prepare_image_process PRIVATE
        RUFUS_PREPARE_HELPER="$<TARGET_FILE:rufus-prepare-helper>"
        RUFUS_PREPARE_PROBE="$<TARGET_FILE:prepare_helper_probe>")
    target_link_libraries(test_prepare_image_process PRIVATE rufus_prepare_process Qt6::Test)
    add_dependencies(test_prepare_image_process rufus-prepare-helper prepare_helper_probe)
    add_test(NAME rufus_prepare_process COMMAND test_prepare_image_process)
    set_tests_properties(rufus_prepare_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30)
    add_executable(test_freedos_request tests/test_freedos_request.cpp)
    set_target_properties(test_freedos_request PROPERTIES AUTOMOC ON)
    target_compile_options(test_freedos_request PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_freedos_request PRIVATE RUFUS_PREPARE_PROBE="$<TARGET_FILE:prepare_helper_probe>")
    target_link_libraries(test_freedos_request PRIVATE rufus_prepare_process rufus_format_options
        rufus_partition_image rufus_fat_format rufus_qt_reporting Qt6::Test)
    target_link_options(test_freedos_request PRIVATE
        -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_dependencies(test_freedos_request prepare_helper_probe)
    add_test(NAME rufus_freedos_request COMMAND test_freedos_request)
    set_tests_properties(rufus_freedos_request PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30)
    add_executable(test_syslinux_request tests/test_syslinux_request.cpp)
    set_target_properties(test_syslinux_request PROPERTIES AUTOMOC ON)
    target_compile_options(test_syslinux_request PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_syslinux_request PRIVATE rufus_prepare_process rufus_format_options
        rufus_partition_image rufus_fat_format rufus_qt_reporting Qt6::Test)
    target_link_options(test_syslinux_request PRIVATE
        -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_test(NAME rufus_syslinux_request COMMAND test_syslinux_request)
    set_tests_properties(rufus_syslinux_request PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30)
    add_executable(test_grub2_request tests/test_grub2_request.cpp)
    set_target_properties(test_grub2_request PROPERTIES AUTOMOC ON)
    target_compile_options(test_grub2_request PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_grub2_request PRIVATE rufus_prepare_process rufus_format_options
        rufus_partition_image rufus_fat_format rufus_qt_reporting Qt6::Test)
    target_link_options(test_grub2_request PRIVATE
        -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_test(NAME rufus_grub2_request COMMAND test_grub2_request)
    add_executable(test_windows_bios_request tests/test_windows_bios_request.cpp)
    set_target_properties(test_windows_bios_request PROPERTIES AUTOMOC ON)
    target_compile_options(test_windows_bios_request PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_windows_bios_request PRIVATE RUFUS_PREPARE_PROBE="$<TARGET_FILE:prepare_helper_probe>"
        RUFUS_ISO_HELPER_FIXTURE="$<TARGET_FILE:iso_helper_fixture>")
    target_link_libraries(test_windows_bios_request PRIVATE rufus_prepare_process rufus_format_options
        rufus_partition_image rufus_prepare_source rufus_qt_reporting Qt6::Test)
    target_link_options(test_windows_bios_request PRIVATE
        -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_dependencies(test_windows_bios_request prepare_helper_probe iso_helper_fixture)
    add_test(NAME rufus_windows_bios_request COMMAND test_windows_bios_request)
    set_tests_properties(rufus_windows_bios_request PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30)
    # Its staging cleanup inventory must not include concurrent GUI jobs.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tmp/grub2-request")
    set_tests_properties(rufus_grub2_request PROPERTIES
        ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-tmp/grub2-request"
        LABELS "functional;process;no-hardware" TIMEOUT 30)
    add_executable(prepare_grub2_image_probe helper/prepare_main.cpp helper/boot_resources.qrc
        tests/prepare_grub2_image_target.cpp)
    set_target_properties(prepare_grub2_image_probe PROPERTIES AUTOMOC ON AUTORCC ON)
    target_compile_definitions(prepare_grub2_image_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
        RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}" RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_compile_options(prepare_grub2_image_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(prepare_grub2_image_probe PRIVATE rufus_format_options rufus_iso_write_check
        rufus_raw_device rufus_prepare_source rufus_prepare_device rufus_qt_reporting
        rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
    target_link_options(prepare_grub2_image_probe PRIVATE -Wl,--wrap=fstatfs64 -Wl,--wrap=openat64 -Wl,--wrap=getuid -Wl,--wrap=geteuid
        -Wl,--wrap=unshare -Wl,--wrap=mount -Wl,--wrap=ioctl -Wl,--wrap=InspectWritableTarget
        -Wl,--wrap=WriteGrub2CoreImage -Wl,--wrap=WriteGrub4dosSbrImage
        -Wl,--wrap=_ZN5rufus23ReadEmbeddedGrub4dosSbrEyR10QByteArray -Wl,--wrap=ExtractIsoTree -Wl,--wrap=WriteMbrUefiNtfsImage
        -Wl,--wrap=InstallFatDeviceBootRecords -Wl,--wrap=InstallLegacyFatDeviceBootRecords -Wl,--wrap=CreateMbrImage
        -Wl,--wrap=pwrite64 -Wl,--wrap=fsync
        -Wl,--wrap=AnalyzeIsoImageWithSource)
    add_executable(test_prepare_grub2_image tests/test_prepare_grub2_image.cpp)
    set_target_properties(test_prepare_grub2_image PROPERTIES AUTOMOC ON)
    target_compile_options(test_prepare_grub2_image PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_prepare_grub2_image PRIVATE RUFUS_MKISOFS="${RUFUS_MKISOFS}"
        RUFUS_UEFI_NTFS_PAYLOAD="${CMAKE_CURRENT_SOURCE_DIR}/../res/uefi/uefi-ntfs.img"
        RUFUS_GRUB_IMAGE_PROBE="$<TARGET_FILE:prepare_grub2_image_probe>"
        RUFUS_GRUB4DOS_LOADER="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub4dos-0.4.6a/grldr"
        RUFUS_GRUB4DOS_MBR="${CMAKE_CURRENT_SOURCE_DIR}/../res/grub/grldr.mbr"
        RUFUS_WIM_FIXTURE="$<TARGET_FILE:wim_fixture>")
    target_link_libraries(test_prepare_grub2_image PRIVATE rufus_prepare_source rufus_qt_reporting rufus_wimlib Qt6::Test)
    add_dependencies(test_prepare_grub2_image prepare_grub2_image_probe wim_fixture)
    add_test(NAME rufus_prepare_grub2_image COMMAND test_prepare_grub2_image)
    set_tests_properties(rufus_prepare_grub2_image PROPERTIES LABELS "functional;image;process;no-hardware" TIMEOUT 600)
    add_executable(test_raw_helper_process tests/test_raw_helper_process.cpp)
    set_target_properties(test_raw_helper_process PROPERTIES AUTOMOC ON)
    target_compile_options(test_raw_helper_process PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_raw_helper_process PRIVATE rufus_prepare_process Qt6::Test)
    target_link_options(test_raw_helper_process PRIVATE -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_test(NAME rufus_raw_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_raw_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_XZ_PROTOCOL=0")
    add_test(NAME rufus_xz_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_xz_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_XZ_PROTOCOL=1")
    add_test(NAME rufus_bzip2_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_bzip2_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_BZIP2_PROTOCOL=1")
    add_test(NAME rufus_lzma_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_lzma_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_LZMA_PROTOCOL=1")
    add_test(NAME rufus_lzw_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_lzw_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_LZW_PROTOCOL=1")
    add_test(NAME rufus_zip_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_zip_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_ZIP_PROTOCOL=1")
    add_test(NAME rufus_zstd_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_zstd_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_ZSTD_PROTOCOL=1")
    add_test(NAME rufus_vtsi_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_vtsi_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_VTSI_PROTOCOL=1")
    add_test(NAME rufus_ffu_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_ffu_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_FFU_PROTOCOL=1")
    add_test(NAME rufus_vhd_helper_process COMMAND test_raw_helper_process)
    set_tests_properties(rufus_vhd_helper_process PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 30
        ENVIRONMENT "RUFUS_TEST_VHD_PROTOCOL=1")
    add_executable(compressed_device_helper_probe helper/prepare_main.cpp helper/boot_resources.qrc
        tests/compressed_device_fixture.cpp)
    set_target_properties(compressed_device_helper_probe PROPERTIES AUTOMOC ON AUTORCC ON)
    target_compile_definitions(compressed_device_helper_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
        RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}" RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
    target_compile_options(compressed_device_helper_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(compressed_device_helper_probe PRIVATE rufus_format_options rufus_iso_write_check
        rufus_raw_device rufus_prepare_source rufus_prepare_device rufus_qt_reporting
        rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
    target_link_options(compressed_device_helper_probe PRIVATE -Wl,--wrap=getuid -Wl,--wrap=geteuid
        -Wl,--wrap=open64 -Wl,--wrap=close -Wl,--wrap=fstat64 -Wl,--wrap=ioctl
        -Wl,--wrap=pread64 -Wl,--wrap=pwrite64 -Wl,--wrap=fsync -Wl,--wrap=WriteCompressedImageToDevice
        -Wl,--wrap=_ZN5rufus25ReadPrepareDeviceSnapshotERK15QDBusConnectionRKNS_19PrepareImageRequestER4QMapI15QDBusObjectPathS6_I7QStringS6_IS8_8QVariantEEERNS_6DeviceE
        -Wl,--wrap=_ZN5rufus27CheckPrepareDeviceSelectionERK15QDBusConnectionRKNS_19PrepareImageRequestEbPi
        -Wl,--wrap=_ZN5rufus22ListUDisksTargetMountsERK4QMapI15QDBusObjectPathS0_I7QStringS0_IS2_8QVariantEEERKNS_6DeviceEyR5QListIS2_EbNS_12DeviceAccessE
        -Wl,--wrap=_ZN5rufus23MatchUDisksTargetMountsERK4QMapI15QDBusObjectPathS0_I7QStringS0_IS2_8QVariantEEERKNS_6DeviceEyyRKNS_18NativeTargetMountsEbNS_12DeviceAccessE
        -Wl,--wrap=_ZN5rufus20OpenNativeCallerHomeEjRi
        -Wl,--wrap=_ZN5rufus23CheckNativeTargetMountsEiiRNS_18NativeTargetMountsEi
        -Wl,--wrap=_ZN5rufus19UnmountNativeTargetEiiRKNS_18NativeTargetMountsERii)
    add_test(NAME rufus_compressed_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>")
    set_tests_properties(rufus_compressed_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_xz_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --xz)
    set_tests_properties(rufus_compressed_xz_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_bzip2_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --bzip2)
    set_tests_properties(rufus_compressed_bzip2_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_lzma_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --lzma)
    set_tests_properties(rufus_compressed_lzma_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_lzw_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --lzw)
    set_tests_properties(rufus_compressed_lzw_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_lzw_gzip_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --lzw-gzip)
    set_tests_properties(rufus_compressed_lzw_gzip_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_zip_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --zip)
    set_tests_properties(rufus_compressed_zip_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_zstd_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>" --zstd)
    set_tests_properties(rufus_compressed_zstd_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_vtsi_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_vtsi_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>")
    set_tests_properties(rufus_compressed_vtsi_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_compressed_vhd_device_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_vhd_device_helper.py" "$<TARGET_FILE:compressed_device_helper_probe>")
    set_tests_properties(rufus_compressed_vhd_device_helper PROPERTIES LABELS "functional;image;write;process;no-hardware" TIMEOUT 90)
    add_test(NAME rufus_prepare_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_helper.py" "$<TARGET_FILE:rufus-prepare-helper>"
        --probe "$<TARGET_FILE:prepare_helper_probe>" --sfdisk "${RUFUS_SFDISK}"
        --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}")
    set_tests_properties(rufus_prepare_helper PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 60)
    add_test(NAME rufus_prepare_fat16_helper COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_helper.py" "$<TARGET_FILE:rufus-prepare-helper>"
        --probe "$<TARGET_FILE:prepare_helper_probe>" --sfdisk "${RUFUS_SFDISK}"
        --fsck "${RUFUS_FSCK_FAT}" --mdir "${RUFUS_MDIR}" --fat16)
    set_tests_properties(rufus_prepare_fat16_helper PROPERTIES LABELS "functional;process;no-hardware" TIMEOUT 60)
    if(RUFUS_BUILD_VM_TEST)
        add_executable(test_discovery_guest tests/vm/test_discovery_guest.cpp)
        target_compile_options(test_discovery_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_discovery_guest PRIVATE rufus_udisks Qt6::Test)
        find_program(RUFUS_FIXTURE_NASM nasm)
        if(RUFUS_FIXTURE_NASM)
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/windows-bootmgr.bin"
                    "${CMAKE_CURRENT_BINARY_DIR}/windows-second-disk.bin"
                COMMAND "${RUFUS_FIXTURE_NASM}" -f bin
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/windows_bootmgr.asm"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/windows-bootmgr.bin"
                COMMAND "${RUFUS_FIXTURE_NASM}" -f bin -DSECOND_DISK
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/windows_bootmgr.asm"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/windows-second-disk.bin"
                DEPENDS tests/vm/windows_bootmgr.asm VERBATIM)
            add_custom_target(windows_bios_handoff_fixture DEPENDS
                "${CMAKE_CURRENT_BINARY_DIR}/windows-bootmgr.bin"
                "${CMAKE_CURRENT_BINARY_DIR}/windows-second-disk.bin")
        endif()
        find_program(RUFUS_FIXTURE_CLANG clang)
        find_program(RUFUS_FIXTURE_LLD lld-link)
        if(RUFUS_FIXTURE_CLANG AND RUFUS_FIXTURE_LLD)
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/uefi-handoff.efi"
                COMMAND "${RUFUS_FIXTURE_CLANG}" --target=x86_64-pc-win32-coff
                    -ffreestanding -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra
                    -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/uefi_handoff.c"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/uefi-handoff.obj"
                COMMAND "${RUFUS_FIXTURE_LLD}" /subsystem:efi_application /entry:efi_main
                    /nodefaultlib /machine:x64 /base:0x100000 /dynamicbase
                    "/out:${CMAKE_CURRENT_BINARY_DIR}/uefi-handoff.efi"
                    "${CMAKE_CURRENT_BINARY_DIR}/uefi-handoff.obj"
                DEPENDS tests/vm/uefi_handoff.c VERBATIM)
            add_custom_target(uefi_handoff_fixture DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/uefi-handoff.efi")
        endif()
        add_executable(prepare_syslinux_probe helper/prepare_main.cpp helper/boot_resources.qrc tests/prepare_syslinux_faults.cpp)
        set_target_properties(prepare_syslinux_probe PROPERTIES AUTOMOC ON AUTORCC ON)
        target_compile_definitions(prepare_syslinux_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
            RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
            RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
        target_compile_options(prepare_syslinux_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(prepare_syslinux_probe PRIVATE rufus_iso_write_check rufus_raw_device rufus_prepare_source
            rufus_prepare_device rufus_qt_reporting rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
        target_link_options(prepare_syslinux_probe PRIVATE -Wl,--wrap=RunInstallerCommand -Wl,--wrap=fsync
            -Wl,--wrap=ExtractIsoTree -Wl,--wrap=WriteMbrUefiNtfsImage
            -Wl,--wrap=pread64 -Wl,--wrap=_ZN13QTemporaryDir6removeEv -Wl,--wrap=mount
            -Wl,--wrap=syncfs -Wl,--wrap=umount -Wl,--wrap=rmdir -Wl,--wrap=fsetxattr)
        add_executable(test_target_mounts_guest tests/vm/test_target_mounts_guest.cpp)
        target_compile_definitions(test_target_mounts_guest PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
        target_compile_options(test_target_mounts_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_target_mounts_guest PRIVATE rufus_target_mounts PkgConfig::RUFUS_MOUNT)
        target_link_options(test_target_mounts_guest PRIVATE -Wl,--wrap=umount2 -Wl,--wrap=mnt_table_parse_file
            -Wl,--wrap=unshare -Wl,--wrap=mount)
        add_executable(prepare_exfat_probe helper/prepare_main.cpp helper/boot_resources.qrc tests/prepare_exfat_faults.cpp)
        set_target_properties(prepare_exfat_probe PROPERTIES AUTOMOC ON AUTORCC ON)
        target_compile_definitions(prepare_exfat_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
            RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
            RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
        target_compile_options(prepare_exfat_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(prepare_exfat_probe PRIVATE -Wl,--wrap=RunFormatCommand -Wl,--wrap=fsync
            -Wl,--wrap=unshare -Wl,--wrap=mount -Wl,--wrap=WriteUefiNtfsImage -Wl,--wrap=WriteMbrUefiNtfsImage
            -Wl,--wrap=WriteGrub2CoreImage -Wl,--wrap=InstallFatDeviceBootRecords
            -Wl,--wrap=_ZN5rufus21InstallNtfsBootRecordEiyRKNS_17NtfsFormatOptionsERNS_14NtfsBootResultE
            -Wl,--wrap=pwrite64 -Wl,--wrap=pread64)
        target_link_libraries(prepare_exfat_probe PRIVATE rufus_iso_write_check rufus_raw_device rufus_prepare_source
            rufus_prepare_device rufus_qt_reporting rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
        add_executable(prepare_boot_probe helper/prepare_main.cpp helper/boot_resources.qrc tests/prepare_boot_faults.cpp)
        target_link_libraries(prepare_boot_probe PRIVATE rufus_iso_write_check rufus_raw_device)
        set_target_properties(prepare_boot_probe PROPERTIES AUTOMOC ON AUTORCC ON)
        target_compile_definitions(prepare_boot_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
            RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
            RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
        target_compile_options(prepare_boot_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(prepare_boot_probe PRIVATE -Wl,--wrap=WriteUefiNtfsImage -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
        target_link_libraries(prepare_boot_probe PRIVATE rufus_prepare_source rufus_prepare_device
            rufus_qt_reporting rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
        add_executable(prepare_extract_probe helper/prepare_main.cpp helper/boot_resources.qrc tests/prepare_extract_faults.c)
        target_link_libraries(prepare_extract_probe PRIVATE rufus_iso_write_check rufus_raw_device)
        set_target_properties(prepare_extract_probe PROPERTIES AUTOMOC ON AUTORCC ON)
        target_compile_definitions(prepare_extract_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
            RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
            RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
        target_compile_options(prepare_extract_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(prepare_extract_probe PRIVATE -Wl,--wrap=write -Wl,--wrap=syncfs -Wl,--wrap=umount -Wl,--wrap=mount)
        target_link_libraries(prepare_extract_probe PRIVATE rufus_prepare_source rufus_prepare_device
            rufus_qt_reporting rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
        add_executable(test_prepare_device_guest tests/vm/test_prepare_device_guest.cpp)
        set_target_properties(test_prepare_device_guest PROPERTIES AUTOMOC ON)
        target_compile_options(test_prepare_device_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_prepare_device_guest PRIVATE rufus_prepare_device rufus_prepare_source rufus_qt_widgets Qt6::Test)
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
        add_executable(test_mbr_guest tests/vm/test_mbr_guest.c)
        target_compile_definitions(test_mbr_guest PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
        target_compile_options(test_mbr_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_mbr_guest PRIVATE rufus_partition_image)
        add_executable(test_persistence_guest tests/vm/test_persistence_guest.cpp)
        target_link_libraries(test_persistence_guest PRIVATE rufus_qt_widgets rufus_prepare_device rufus_uefi_ntfs Qt6::Test)
        target_link_options(test_persistence_guest PRIVATE -Wl,--wrap=getuid)
        target_compile_options(test_persistence_guest PRIVATE -Wall -Wextra -Wpedantic)
        add_executable(test_mbr_handoff_guest tests/vm/test_mbr_handoff_guest.cpp)
        add_executable(test_device_source_qt_guest tests/vm/test_device_source_qt_guest.cpp)
        target_compile_features(test_device_source_qt_guest PRIVATE cxx_std_17)
        target_link_libraries(test_device_source_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_device_source_qt_guest PRIVATE -Wl,--wrap=getuid -Wl,--wrap=geteuid)
        add_executable(test_native_format_qt_guest tests/vm/test_native_format_qt_guest.cpp)
        target_compile_features(test_native_format_qt_guest PRIVATE cxx_std_17)
        target_link_libraries(test_native_format_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_native_format_qt_guest PRIVATE -Wl,--wrap=getuid)
        add_executable(test_media_appearance_qt_guest tests/vm/test_media_appearance_qt_guest.cpp)
        add_executable(test_archive_qt_guest tests/vm/test_archive_qt_guest.cpp)
        add_executable(windows_to_go_fixture tests/windows_to_go_fixture_main.cpp)
        target_link_libraries(windows_to_go_fixture PRIVATE rufus_windows_to_go_fixture)
        add_executable(test_windows_to_go_qt_guest tests/vm/test_windows_to_go_qt_guest.cpp)
        target_compile_options(test_windows_to_go_qt_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_windows_to_go_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_windows_to_go_qt_guest PRIVATE -Wl,--wrap=getuid)
        target_compile_options(test_archive_qt_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_archive_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_archive_qt_guest PRIVATE -Wl,--wrap=getuid)
        target_compile_options(test_media_appearance_qt_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_media_appearance_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_media_appearance_qt_guest PRIVATE -Wl,--wrap=getuid)
        add_executable(test_compressed_qt_guest tests/vm/test_compressed_qt_guest.cpp)
        add_executable(save_guest_launcher tests/vm/save_guest_launcher.c)
        target_compile_options(save_guest_launcher PRIVATE -fno-sanitize=all)
        target_link_options(save_guest_launcher PRIVATE -fno-sanitize=all)
        add_executable(rufus-save-guest-helper tests/vm/save_guest_helper.cpp platform/save_source.cpp)
        set_target_properties(rufus-save-guest-helper PROPERTIES AUTOMOC ON)
        target_compile_definitions(rufus-save-guest-helper PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 RUFUS_SAVE_GUEST_TEST
            RUFUS_NTFS_3G_PATH="${RUFUS_NTFS_3G}")
        target_link_options(rufus-save-guest-helper PRIVATE -Wl,--wrap=umount)
        target_link_libraries(rufus-save-guest-helper PRIVATE rufus_device_selection rufus_target_mounts rufus_image_save rufus_qt_reporting)
        add_executable(test_image_save_qt_guest tests/vm/test_image_save_qt_guest.cpp)
        target_compile_options(test_image_save_qt_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_image_save_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_compile_options(test_compressed_qt_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_compressed_qt_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_compressed_qt_guest PRIVATE -Wl,--wrap=getuid)
        add_executable(compressed_helper_guest_probe helper/prepare_main.cpp helper/boot_resources.qrc
            tests/vm/compressed_helper_faults.cpp)
        set_target_properties(compressed_helper_guest_probe PROPERTIES AUTOMOC ON AUTORCC ON)
        target_compile_definitions(compressed_helper_guest_probe PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
            RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}" RUFUS_UEFI_NTFS_BYTES=${RUFUS_UEFI_NTFS_BYTES})
        target_compile_options(compressed_helper_guest_probe PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(compressed_helper_guest_probe PRIVATE rufus_format_options rufus_iso_write_check
            rufus_raw_device rufus_prepare_source rufus_prepare_device rufus_qt_reporting
            rufus_partition_image rufus_uefi_ntfs rufus_fat_boot rufus_freedos_copy)
        target_link_options(compressed_helper_guest_probe PRIVATE -Wl,--wrap=uname -Wl,--wrap=TestBadBlocksDevice -Wl,--wrap=WriteCompressedImageToDevice
            -Wl,--wrap=pread64 -Wl,--wrap=pwrite64 -Wl,--wrap=fsync -Wl,--wrap=close)
        add_executable(test_grub2_widgets_guest tests/vm/test_grub2_widgets_guest.cpp)
        target_link_libraries(test_grub2_widgets_guest PRIVATE rufus_qt_widgets Qt6::Test)
        target_link_options(test_grub2_widgets_guest PRIVATE -Wl,--wrap=getuid
            -Wl,--wrap=_ZN12QApplication4beepEv -Wl,--wrap=_ZN12QApplication5alertEP7QWidgeti)
        target_compile_options(test_grub2_widgets_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_definitions(test_mbr_handoff_guest PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
            RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
            RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}")
        target_compile_options(test_mbr_handoff_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_mbr_handoff_guest PRIVATE rufus_prepare_device rufus_partition_image rufus_format_options
            rufus_syslinux_install rufus_iso_extract rufus_prepare_source rufus_raw_device rufus_image_save Qt6::Test)
        target_link_options(test_mbr_handoff_guest PRIVATE -Wl,--wrap=FormatFat32Partition -Wl,--wrap=FormatFat16Partition
            -Wl,--wrap=pread64 -Wl,--wrap=mount -Wl,--wrap=syncfs -Wl,--wrap=fsync -Wl,--wrap=RunInstallerCommand
            -Wl,--wrap=RunFormatCommand -Wl,--wrap=umount2)
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
        -Wl,--wrap=OpenWimFromIsoReader -Wl,--wrap=ReadWimMetadata -Wl,--wrap=PrepareCompressedWrite)
    add_dependencies(test_image_analysis wim_fixture)
    target_link_libraries(test_image_analysis PRIVATE rufus_qt_widgets Qt6::Test)
    add_test(NAME rufus_image_analysis_widgets COMMAND test_image_analysis)
    set_tests_properties(rufus_image_analysis_widgets PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;widgets;image;no-hardware"
        TIMEOUT 60
    )
    add_executable(test_image_hash_widgets tests/test_image_hash_widgets.cpp)
    set_target_properties(test_image_hash_widgets PROPERTIES AUTOMOC ON)
    target_compile_definitions(test_image_hash_widgets PRIVATE RUFUS_MKISOFS="${RUFUS_MKISOFS}")
    target_compile_options(test_image_hash_widgets PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_image_hash_widgets PRIVATE rufus_qt_widgets Qt6::Test)
    target_link_options(test_image_hash_widgets PRIVATE -Wl,--wrap=HashImageFileSource
        -Wl,--wrap=CloseImageFileSource -Wl,--wrap=_ZN12QApplication4beepEv
        -Wl,--wrap=_ZN12QApplication5alertEP7QWidgeti
        -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_test(NAME rufus_image_hash_widgets COMMAND test_image_hash_widgets)
    add_test(NAME rufus_image_hash_widgets_scaled COMMAND test_image_hash_widgets largeFontDialog)
    set_tests_properties(rufus_image_hash_widgets rufus_image_hash_widgets_scaled PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "functional;widgets;image;no-hardware" TIMEOUT 60)
    set_property(TEST rufus_image_hash_widgets_scaled APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")
    add_executable(test_image_mode_dialog tests/test_image_mode_dialog.cpp)
    add_executable(test_windows_user_dialog tests/test_windows_user_dialog.cpp)
    set_target_properties(test_windows_user_dialog PROPERTIES AUTOMOC ON)
    target_compile_options(test_windows_user_dialog PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_windows_user_dialog PRIVATE rufus_qt_widgets Qt6::Test)
    add_test(NAME rufus_windows_user_dialog COMMAND test_windows_user_dialog)
    add_test(NAME rufus_windows_user_dialog_scaled COMMAND test_windows_user_dialog keyboardAndLargeFontLayout)
    set_tests_properties(rufus_windows_user_dialog rufus_windows_user_dialog_scaled PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "functional;widgets;image;no-hardware" TIMEOUT 10)
    set_property(TEST rufus_windows_user_dialog_scaled APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")
    add_executable(test_windows_region_dialog tests/test_windows_region_dialog.cpp)
    set_target_properties(test_windows_region_dialog PROPERTIES AUTOMOC ON)
    target_compile_options(test_windows_region_dialog PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_windows_region_dialog PRIVATE rufus_qt_widgets Qt6::Test)
    add_test(NAME rufus_windows_region_dialog COMMAND test_windows_region_dialog)
    add_test(NAME rufus_windows_region_dialog_scaled COMMAND test_windows_region_dialog keyboardAndLargeFontLayout)
    set_tests_properties(rufus_windows_region_dialog rufus_windows_region_dialog_scaled PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "functional;widgets;image;no-hardware" TIMEOUT 30)
    set_property(TEST rufus_windows_region_dialog_scaled APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")
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

    add_test(NAME rufus_format_geometry COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_format_geometry.py"
        --fat-probe "$<TARGET_FILE:fat_format_probe>" --ntfs-probe "$<TARGET_FILE:ntfs_format_probe>"
        --exfat-probe "$<TARGET_FILE:exfat_format_probe>" --ext-probe "$<TARGET_FILE:ext_format_probe>"
        --fat-boot-probe "$<TARGET_FILE:freedos_boot_probe>" --ntfs-boot-probe "$<TARGET_FILE:ntfs_boot_probe>"
        --mkfs-fat "${RUFUS_MKFS_FAT}" --mkfs-ntfs "${RUFUS_MKNTFS}"
        --mkfs-exfat "${RUFUS_MKFS_EXFAT}" --mkfs-ext "${RUFUS_MKE2FS}")
    set_tests_properties(rufus_format_geometry PROPERTIES LABELS "functional;format;no-hardware" TIMEOUT 180)

    add_executable(test_filesystem_batch tests/test_filesystem_batch.cpp)
    set_target_properties(test_filesystem_batch PROPERTIES AUTOMOC ON)
    target_compile_features(test_filesystem_batch PRIVATE cxx_std_17)
    target_compile_definitions(test_filesystem_batch PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(test_filesystem_batch PRIVATE rufus_format_options rufus_udf_format rufus_qt_reporting Qt6::Test)
    target_link_options(test_filesystem_batch PRIVATE -Wl,--wrap=IsRufusJobCancelled)
    target_compile_options(test_filesystem_batch PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_filesystem_batch COMMAND test_filesystem_batch)
    set_tests_properties(rufus_filesystem_batch PROPERTIES LABELS "component;format;no-hardware" TIMEOUT 90)

    add_executable(test_application tests/test_application.cpp)
    set_target_properties(test_application PROPERTIES AUTOMOC ON)
    target_link_libraries(test_application PRIVATE rufus_application Qt6::Test)
    target_compile_options(test_application PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_application COMMAND test_application)
    set_tests_properties(rufus_application PROPERTIES LABELS "component;application;no-hardware" TIMEOUT 15)

    add_executable(test_desktop_controls tests/test_desktop_controls.cpp tests/http_fixture.h)
    set_target_properties(test_desktop_controls PROPERTIES AUTOMOC ON)
    target_link_libraries(test_desktop_controls PRIVATE rufus_qt_widgets Qt6::Test)
    target_compile_definitions(test_desktop_controls PRIVATE RUFUS_PREPARE_HELPER="$<TARGET_FILE:rufus-prepare-helper>")
    add_dependencies(test_desktop_controls rufus-prepare-helper)
    target_compile_options(test_desktop_controls PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_desktop_controls COMMAND test_desktop_controls)
    set_tests_properties(rufus_desktop_controls PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "functional;widgets;no-hardware" TIMEOUT 40)
    add_executable(test_rufus_window tests/test_rufus_window.cpp tests/http_fixture.h)
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
    add_executable(test_device_options tests/test_device_options.cpp)
    set_target_properties(test_device_options PROPERTIES AUTOMOC ON)
    target_link_libraries(test_device_options PRIVATE rufus_qt_widgets Qt6::Test)
    target_compile_options(test_device_options PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_device_options COMMAND test_device_options)
    set_tests_properties(rufus_device_options PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_device_discovery tests/test_device_discovery.cpp)
    set_target_properties(test_device_discovery PROPERTIES AUTOMOC ON)
    target_link_libraries(test_device_discovery PRIVATE rufus_udisks rufus_qt_widgets Qt6::Test)
    target_compile_options(test_device_discovery PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_device_discovery COMMAND test_device_discovery)
    set_tests_properties(rufus_device_discovery PROPERTIES LABELS "component;no-hardware" TIMEOUT 15)
    add_executable(download_provider_probe tests/download_provider_probe.cpp)
    set_target_properties(download_provider_probe PROPERTIES AUTOMOC ON)
    target_link_libraries(download_provider_probe PRIVATE rufus_image_download)
    add_executable(application_update_probe tests/application_update_probe.cpp)
    set_target_properties(application_update_probe PROPERTIES AUTOMOC ON)
    target_link_libraries(application_update_probe PRIVATE rufus_application_updates)
    target_include_directories(application_update_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_executable(test_iso_options tests/test_iso_options.cpp)
    add_executable(test_ntfs_options tests/test_ntfs_options.cpp)
    set_target_properties(test_ntfs_options PROPERTIES AUTOMOC ON)
    target_link_libraries(test_ntfs_options PRIVATE rufus_qt_widgets rufus_ntfs_format Qt6::Test)
    target_compile_definitions(test_ntfs_options PRIVATE RUFUS_MKNTFS="${RUFUS_MKNTFS}" RUFUS_NTFSINFO="${RUFUS_NTFSINFO}")
    target_compile_options(test_ntfs_options PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_ntfs_options COMMAND test_ntfs_options)
    set_tests_properties(rufus_ntfs_options PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_target_properties(test_iso_options PROPERTIES AUTOMOC ON)
    target_link_libraries(test_iso_options PRIVATE rufus_qt_widgets rufus_iso_extract Qt6::Test)
    target_compile_definitions(test_iso_options PRIVATE RUFUS_MKISOFS="${RUFUS_MKISOFS}" RUFUS_PREPARE_HELPER="$<TARGET_FILE:rufus-prepare-helper>")
    target_compile_options(test_iso_options PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_iso_options COMMAND test_iso_options)
    set_tests_properties(rufus_iso_options PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_application_updates tests/test_application_updates.cpp tests/http_fixture.h)
    set_target_properties(test_application_updates PROPERTIES AUTOMOC ON)
    target_link_libraries(test_application_updates PRIVATE rufus_qt_widgets Qt6::Test)
    target_compile_options(test_application_updates PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_application_updates COMMAND test_application_updates)
    set_tests_properties(rufus_application_updates PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_localization tests/test_localization.cpp)
    set_target_properties(test_localization PROPERTIES AUTOMOC ON)
    target_link_libraries(test_localization PRIVATE rufus_qt_widgets Qt6::Test)
    target_compile_options(test_localization PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_localization COMMAND test_localization)
    set_tests_properties(rufus_localization PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_download_workflow tests/test_download_workflow.cpp tests/http_fixture.h)
    set_target_properties(test_download_workflow PROPERTIES AUTOMOC ON)
    target_link_libraries(test_download_workflow PRIVATE rufus_qt_widgets Qt6::Test)
    target_link_options(test_download_workflow PRIVATE -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=renameat2)
    target_compile_options(test_download_workflow PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_download_workflow PRIVATE RUFUS_FIDO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fido")
    add_test(NAME rufus_download_workflow COMMAND test_download_workflow)
    set_tests_properties(rufus_download_workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "functional;widgets;network;no-hardware" TIMEOUT 600)
    add_executable(test_udisks_client tests/test_udisks_client.cpp)
    # Only invented-metadata tests replace the native reader; production and VM
    # targets always read the actual USB sysfs snapshot.
    add_library(test_native_disk_fixture OBJECT tests/native_disk_fixture.cpp)
    target_link_libraries(test_native_disk_fixture PRIVATE rufus_native_disk_facts)
    add_executable(test_udisks_mounts tests/test_udisks_mounts.cpp)
    set_target_properties(test_udisks_mounts PROPERTIES AUTOMOC ON)
    target_link_libraries(test_udisks_mounts PRIVATE rufus_udisks Qt6::Test)
    target_compile_options(test_udisks_mounts PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rufus_udisks_mounts COMMAND test_udisks_mounts)
    set_tests_properties(rufus_udisks_mounts PROPERTIES LABELS "component;no-hardware" TIMEOUT 10)
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
    add_executable(iso_helper_fixture tests/iso_helper_fixture.cpp)
    target_link_libraries(iso_helper_fixture PRIVATE rufus_prepare_process rufus_format_options)
    target_compile_options(iso_helper_fixture PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(test_raw_write_widgets PROPERTIES AUTOMOC ON)
    target_link_libraries(test_raw_write_widgets PRIVATE rufus_qt_widgets rufus_windows_to_go_fixture Qt6::Test)
    target_compile_definitions(test_raw_write_widgets PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
        RUFUS_SIGNED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/download/grub-2.12"
        RUFUS_MKE2FS_PATH="${RUFUS_MKE2FS}"
        RUFUS_MKFS_FAT_PATH="${RUFUS_MKFS_FAT}" RUFUS_MKNTFS_PATH="${RUFUS_MKNTFS}"
        RUFUS_DBUS_DAEMON="${RUFUS_DBUS_DAEMON}" RUFUS_MKISOFS="${RUFUS_MKISOFS}"
        RUFUS_SYSLINUX_MODULE="${RUFUS_SYSLINUX_UPSTREAM}/bios/com32/elflink/ldlinux/ldlinux.c32"
        RUFUS_WIM_FIXTURE="$<TARGET_FILE:wim_fixture>")
    target_compile_options(test_raw_write_widgets PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(test_raw_write_widgets PRIVATE -Wl,--wrap=fstat -Wl,--wrap=fstat64 -Wl,--wrap=ioctl
        -Wl,--wrap=access
        -Wl,--wrap=pread -Wl,--wrap=pread64 -Wl,--wrap=pwrite -Wl,--wrap=pwrite64 -Wl,--wrap=fsync)
    target_link_options(test_raw_write_widgets PRIVATE -Wl,--wrap=_ZN5rufus16ReadDiskSequenceEy
        -Wl,--wrap=_ZN12QApplication4beepEv -Wl,--wrap=_ZN12QApplication5alertEP7QWidgeti
        -Wl,--wrap=AnalyzeIsoImageWithSource
        -Wl,--wrap=_ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE)
    add_dependencies(test_raw_write_widgets udisks_fixture iso_helper_fixture wim_fixture)
    add_test(NAME rufus_grub2_widgets COMMAND test_raw_write_widgets
        grubDownloadWarning grubDownloadFlow grubDownloadExcluded
        grubIsoCreation grubIsoFailure grubIsoSelection grubIsoNtfsPolicy grubIsoHybridDd)
    set_tests_properties(rufus_grub2_widgets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;dbus;image;widgets;no-hardware" TIMEOUT 300)
    add_test(NAME rufus_compressed_write_widgets COMMAND test_raw_write_widgets
        zipPickerSelectsCompressedImage compressedPlanGeometry compressedCapacity compressedInvalidatedBeforeAuthorization
        compressedReadyWaitsForJoinedHelper compressedHelperFailure
        mountedRawProgressWaitsForHelperExit mountedRawRouting applicationRemainsGated)
    set_tests_properties(rufus_compressed_write_widgets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;dbus;write;widgets;no-hardware" TIMEOUT 120)
    add_test(NAME rufus_raw_write_widgets COMMAND test_raw_write_widgets)
    # Hundreds of private-bus GUI scenarios, also run under sanitizers. CP54's
    # passing assertions reached the old 600 s deadline during shutdown; allow
    # the full executable to exit without repeating the entire batch gate.
    set_tests_properties(rufus_raw_write_widgets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;dbus;write;widgets;no-hardware" TIMEOUT 900)
    set_target_properties(test_udisks_client PROPERTIES AUTOMOC ON)
    target_link_libraries(test_udisks_client PRIVATE rufus_udisks rufus_qt_widgets Qt6::Test)
    target_compile_definitions(test_udisks_client PRIVATE RUFUS_DBUS_DAEMON="${RUFUS_DBUS_DAEMON}")
    target_compile_options(test_udisks_client PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(test_udisks_client udisks_fixture)
    foreach(target IN ITEMS test_device_options test_udisks_client test_udisks_mounts test_udisks_target test_raw_write_widgets)
        target_link_libraries(${target} PRIVATE test_native_disk_fixture)
        target_link_options(${target} PRIVATE -Wl,--wrap=_ZN5rufus19ReadNativeDiskFactsEyyRNS_15NativeDiskFactsERK7QString)
    endforeach()
    add_test(NAME rufus_udisks COMMAND test_udisks_client)
    set_tests_properties(rufus_udisks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "functional;dbus;no-hardware"
        TIMEOUT 60
    )
endif()
