# SPDX-License-Identifier: GPL-3.0-or-later
# Shared native build for the application and standalone qualification.
find_package(Perl REQUIRED)
get_filename_component(RUFUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

function(add_native_syslinux target upstream major)
    if(major EQUAL 4)
        set(version "4.07")
        set(payload_root "${upstream}")
        set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/v4")
        set(component Syslinux4Runtime)
        set(payloads
            "core/ldlinux.bss|syslinux_bootsect|1|8dc6ec58b4f1df391db68d9e2165ccd47d84f4ba64675f87b677077aae5b53fa"
            "core/ldlinux.sys|syslinux_ldlinux|512|d5b6310740a596ba4367719a3dff4b3442f6da81cbeafbb0bf3fab026d8bc910")
    else()
        set(version "6.04-pre1")
        set(payload_root "${upstream}/bios")
        set(output_dir "${CMAKE_CURRENT_BINARY_DIR}")
        set(component SyslinuxRuntime)
        set(payloads
            "core/ldlinux.bss|syslinux_bootsect|1|cc40ba0349782cb4c9021e54dcc0a4540c3a8b96088b3a5648671926ef44d2f0"
            "core/ldlinux.sys|syslinux_ldlinux|512|73b62767a16200b9af193a7d5c94e9c294c6dbb6d5b17c15038c9f3173c9a7bc"
            "com32/elflink/ldlinux/ldlinux.c32|syslinux_ldlinuxc32|1|d3472c02263acf9cd1da5db51e263c5484bad13ea68618c403d9cb01ca070aee")
    endif()
    file(MAKE_DIRECTORY "${output_dir}")
    if(NOT EXISTS "${payload_root}/version.h")
        message(FATAL_ERROR "Missing syslinux-${version} release header: ${payload_root}/version.h")
    endif()

    # The fixed native installer cannot load another version's payload pair.
    set(generator "${upstream}/libinstaller/bin2c.pl")
    foreach(entry IN LISTS payloads)
        string(REPLACE "|" ";" fields "${entry}")
        list(GET fields 0 path)
        list(GET fields 1 symbol)
        list(GET fields 2 padding)
        list(GET fields 3 expected_hash)
        set(payload "${payload_root}/${path}")
        file(SHA256 "${payload}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Payload does not match qualified ${version}: ${path}")
        endif()
        if(path MATCHES "^core/ldlinux\\.(bss|sys)$")
            set(rufus_payload "${RUFUS_ROOT}/res/syslinux/ldlinux_v${major}.${CMAKE_MATCH_1}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rufus_payload}")
            file(SHA256 "${rufus_payload}" rufus_hash)
            if(NOT rufus_hash STREQUAL actual_hash)
                message(FATAL_ERROR "Rufus v${major} payload differs from upstream: ${path}")
            endif()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${payload}" "${generator}")
        set(output "${output_dir}/${symbol}.c")
        execute_process(COMMAND "${PERL_EXECUTABLE}" "${generator}" "${symbol}" "${padding}"
            INPUT_FILE "${payload}" OUTPUT_FILE "${output}" COMMAND_ERROR_IS_FATAL ANY)
        if(major EQUAL 6)
            file(READ "${output}" embedded)
            string(REPLACE "${symbol}" "rufus_embedded_${symbol}" embedded "${embedded}")
            file(WRITE "${output}" "${embedded}\nunsigned char* ${symbol} = rufus_embedded_${symbol};\nunsigned int ${symbol}_len = sizeof(rufus_embedded_${symbol});\nconst int ${symbol}_mtime = -1;\n")
        endif()
        list(APPEND generated_sources "${output}")
    endforeach()

    set(installer_sources mtools/syslinux.c
        libinstaller/fs.c libinstaller/syslxmod.c libinstaller/syslxopt.c
        libinstaller/setadv.c
        libfat/cache.c libfat/fatchain.c libfat/open.c libfat/searchdir.c)
    if(major EQUAL 6)
        list(INSERT installer_sources 4 libinstaller/syslxrw.c)
    endif()
    set(portable "${output_dir}/portable")
    file(COPY "${upstream}/libfat" DESTINATION "${portable}")
    file(READ "${portable}/libfat/ulint.h" fat_accessors)
    string(REPLACE "#if defined(__i386__) || defined(__x86_64__)" "#if 0 /* native portable access */" fat_accessors "${fat_accessors}")
    string(REPLACE "_p[3] << 24" "(uint32_t)_p[3] << 24" fat_accessors "${fat_accessors}")
    file(WRITE "${portable}/libfat/ulint.h" "${fat_accessors}")
    if(major EQUAL 4)
        # Keep upstream immutable. Its 4.07 portable get_32/get_64/set_64 branches
        # pass integer values where pointers are required and omit high shifts.
        # Compile a private copy with byte-safe little-endian accessors on every
        # host, so native x86 qualification exercises the same path as aarch64.
        set(portable "${output_dir}/portable")
        file(COPY "${upstream}/libinstaller" "${upstream}/mtools" DESTINATION "${portable}")
        file(READ "${upstream}/libinstaller/syslxint.h" accessors)
        string(REPLACE "#if X86_MEM" "#if 0 /* native portable access */" accessors "${accessors}")
        string(REPLACE "get_16(pp[0]) + (uint32_t)get_16(pp[1])" "get_16(pp) | ((uint32_t)get_16(pp + 1) << 16)" accessors "${accessors}")
        string(REPLACE "get_32(pp[0]) + (uint64_t)get_32(pp[1])" "get_32(pp) | ((uint64_t)get_32(pp + 1) << 32)" accessors "${accessors}")
        string(REPLACE "set_32(pp[0], v);" "set_32(pp, v);" accessors "${accessors}")
        string(REPLACE "set_32(pp[1], v >> 32);" "set_32(pp + 1, v >> 32);" accessors "${accessors}")
        file(WRITE "${portable}/libinstaller/syslxint.h" "${accessors}")
        foreach(source IN LISTS installer_sources)
            if(source MATCHES "^(libinstaller|libfat|mtools)/")
                list(APPEND portable_sources "${portable}/${source}")
            else()
                list(APPEND portable_sources "${upstream}/${source}")
            endif()
        endforeach()
        set(installer_sources ${portable_sources})
    else()
        set(portable "${output_dir}/portable")
        file(COPY "${upstream}/libinstaller" "${upstream}/mtools" "${upstream}/extlinux" DESTINATION "${portable}")
        # Version 5 templates also contain unaligned fields inside the loader.
        # Use upstream's byte-safe path on x86 too, as for the v4 provider.
        file(READ "${portable}/libinstaller/syslxint.h" native_accessors)
        string(REPLACE "#if X86_MEM" "#if 0 /* native portable access */" native_accessors "${native_accessors}")
        file(WRITE "${portable}/libinstaller/syslxint.h" "${native_accessors}")
        file(READ "${portable}/libinstaller/syslinux.h" native_header)
        foreach(symbol syslinux_bootsect syslinux_ldlinux syslinux_ldlinuxc32)
            string(REPLACE "${symbol}[]" "*${symbol}" native_header "${native_header}")
            string(REPLACE "const unsigned int ${symbol}_len" "unsigned int ${symbol}_len" native_header "${native_header}")
        endforeach()
        file(WRITE "${portable}/libinstaller/syslinux.h" "${native_header}")
        foreach(source mtools/syslinux.c extlinux/main.c)
            file(READ "${upstream}/${source}" native_main)
            string(REPLACE "int main(int argc, char *argv[])\n{" "extern int RufusLoadSyslinuxResources(void);\nint main(int argc, char *argv[])\n{\n    if (RufusLoadSyslinuxResources() != 0) return 77;" native_main "${native_main}")
            if(source STREQUAL "extlinux/main.c")
                string(REPLACE "    fd = open(c32file, O_WRONLY" "    if (syslinux_ldlinuxc32_len) {\n    fd = open(c32file, O_WRONLY" native_main "${native_main}")
                string(REPLACE "    free(file);\n    free(oldfile);\n    free(c32file);\n    return 0;" "    }\n    free(file);\n    free(oldfile);\n    free(c32file);\n    return 0;" native_main "${native_main}")
                string(REPLACE "    close(fd);\n\n    sync();\n\n    return 0;" "    close(fd);\n    }\n\n    sync();\n\n    return 0;" native_main "${native_main}")
            endif()
            file(WRITE "${portable}/${source}" "${native_main}")
        endforeach()
        foreach(source IN LISTS installer_sources)
            if(source MATCHES "^(libinstaller|mtools|libfat)/")
                list(APPEND native_sources "${portable}/${source}")
            else()
                list(APPEND native_sources "${upstream}/${source}")
            endif()
        endforeach()
        set(installer_sources ${native_sources} "${CMAKE_CURRENT_SOURCE_DIR}/native_resources.c")
    endif()
    # The standalone upstream command leaves its sector map and MTOOLSRC name
    # allocated at successful exit. Release these in both private provider mains.
    file(READ "${portable}/mtools/syslinux.c" native_main)
    string(REPLACE "    close(dev_fd);\n    sync();" "    free(sectors);\n    free(mtools_conf);\n    close(dev_fd);\n    sync();" native_main "${native_main}")
    file(WRITE "${portable}/mtools/syslinux.c" "${native_main}")
    add_executable(${target} ${installer_sources} ${generated_sources})
    set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
    target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    if(major EQUAL 4)
        # Upstream uses _BSD_SOURCE; request its current glibc equivalent as well.
        target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE=1)
    endif()
    target_include_directories(${target} PRIVATE
        "${portable}/libinstaller" "${upstream}/mtools" "${upstream}/libinstaller"
        "${portable}/libfat" "${payload_root}")

    if(major EQUAL 4 AND BUILD_TESTING)
        add_executable(test_syslinux4_accessors "${CMAKE_CURRENT_SOURCE_DIR}/test_accessors.c")
        target_include_directories(test_syslinux4_accessors PRIVATE "${portable}/libinstaller" "${upstream}/libinstaller")
        add_test(NAME rufus_syslinux4_accessors COMMAND test_syslinux4_accessors)
        set_tests_properties(rufus_syslinux4_accessors PROPERTIES LABELS "component;no-hardware" TIMEOUT 10)
    endif()
    install(TARGETS ${target} RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}"
        COMPONENT ${component})
    install(FILES "${upstream}/COPYING" "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${target}" COMPONENT ${component})
    if(major EQUAL 6)
        # Same pinned payloads, native mounted-filesystem provider for NTFS.
        set(extlinux_sources extlinux/main.c extlinux/mountinfo.c
            libinstaller/syslxmod.c libinstaller/syslxopt.c libinstaller/syslxcom.c
            libinstaller/syslxrw.c libinstaller/setadv.c libinstaller/advio.c)
        list(TRANSFORM extlinux_sources PREPEND "${portable}/")
        list(APPEND extlinux_sources "${CMAKE_CURRENT_SOURCE_DIR}/native_resources.c")
        add_executable(rufus-extlinux ${extlinux_sources} ${generated_sources})
        set_target_properties(rufus-extlinux PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
        target_compile_definitions(rufus-extlinux PRIVATE _FILE_OFFSET_BITS=64)
        # major()/minor() moved out of sys/types.h in modern glibc.
        target_compile_options(rufus-extlinux PRIVATE -include sys/sysmacros.h)
        target_include_directories(rufus-extlinux PRIVATE
            "${portable}/extlinux" "${portable}/libinstaller" "${upstream}/libinstaller" "${payload_root}")
        install(TARGETS rufus-extlinux RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}"
            COMPONENT ${component})
    endif()
endfunction()

if(RUFUS_SYSLINUX_UPSTREAM)
    add_native_syslinux(rufus-syslinux "${RUFUS_SYSLINUX_UPSTREAM}" 6)
endif()
if(RUFUS_SYSLINUX4_UPSTREAM)
    add_native_syslinux(rufus-syslinux4 "${RUFUS_SYSLINUX4_UPSTREAM}" 4)
endif()
