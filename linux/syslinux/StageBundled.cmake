# SPDX-License-Identifier: GPL-3.0-or-later
# The original Windows providers stay untouched; native targets use build copies.
find_program(RUFUS_SYSLINUX_PATCH_PROGRAM patch REQUIRED)
set(RUFUS_BUNDLED_SYSLINUX "${CMAKE_CURRENT_BINARY_DIR}/syslinux-bundled/syslinux")
set(syslinux_bundled_root "${CMAKE_CURRENT_SOURCE_DIR}/../src/syslinux")
foreach(component libfat libinstaller)
    file(GLOB_RECURSE syslinux_bundled_inputs CONFIGURE_DEPENDS
        "${syslinux_bundled_root}/${component}/*")
    foreach(syslinux_bundled_input IN LISTS syslinux_bundled_inputs)
        if(NOT IS_DIRECTORY "${syslinux_bundled_input}")
            file(RELATIVE_PATH syslinux_bundled_relative "${syslinux_bundled_root}" "${syslinux_bundled_input}")
            configure_file("${syslinux_bundled_input}" "${RUFUS_BUNDLED_SYSLINUX}/${syslinux_bundled_relative}" COPYONLY)
        endif()
    endforeach()
endforeach()
set(syslinux_bundled_patch "${CMAKE_CURRENT_SOURCE_DIR}/syslinux/bundled-compiler-warnings.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${syslinux_bundled_patch}")
execute_process(COMMAND "${RUFUS_SYSLINUX_PATCH_PROGRAM}" --batch --fuzz=0 -p1
    -i "${syslinux_bundled_patch}" WORKING_DIRECTORY "${RUFUS_BUNDLED_SYSLINUX}"
    RESULT_VARIABLE syslinux_bundled_patch_result OUTPUT_VARIABLE syslinux_bundled_patch_output ERROR_VARIABLE syslinux_bundled_patch_error)
if(NOT syslinux_bundled_patch_result EQUAL 0)
    message(FATAL_ERROR "Bundled Syslinux warning patch failed: ${syslinux_bundled_patch_output}${syslinux_bundled_patch_error}")
endif()
