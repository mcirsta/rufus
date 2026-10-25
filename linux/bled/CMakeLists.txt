# SPDX-License-Identifier: GPL-3.0-or-later
# The original Windows decoders remain unchanged; native targets use these copies.
find_program(RUFUS_BLED_PATCH patch REQUIRED)
get_filename_component(RUFUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(bled_upstream "${RUFUS_ROOT}/src/bled")
set(bled_parent "${CMAKE_CURRENT_BINARY_DIR}/source")
set(bled_source "${bled_parent}/bled")
file(GLOB_RECURSE bled_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false "${bled_upstream}/*")
foreach(input IN LISTS bled_inputs)
    file(RELATIVE_PATH name "${bled_upstream}" "${input}")
    configure_file("${input}" "${bled_source}/${name}" COPYONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS native-warnings.patch)
execute_process(COMMAND "${RUFUS_BLED_PATCH}" --batch --fuzz=0 -p1
    -i "${CMAKE_CURRENT_SOURCE_DIR}/native-warnings.patch"
    WORKING_DIRECTORY "${bled_source}" RESULT_VARIABLE patch_result
    OUTPUT_VARIABLE patch_output ERROR_VARIABLE patch_error)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "Native bled warning patch failed: ${patch_output}${patch_error}")
endif()
set(RUFUS_BLED_SOURCE "${bled_source}" PARENT_SCOPE)
set(RUFUS_BLED_PARENT "${bled_parent}" PARENT_SCOPE)
