# SPDX-License-Identifier: GPL-3.0-or-later
set(reference "${CMAKE_CURRENT_BINARY_DIR}/current_hdd_wrapper.inc")
add_custom_command(OUTPUT "${reference}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/smart.c" "${reference}"
    DEPENDS extract.py ../../../src/smart.c
    VERBATIM)
add_executable(test_hdd_score_reference test_hdd_score.c "${reference}")
target_link_libraries(test_hdd_score_reference PRIVATE rufus_hdd_score)
target_include_directories(test_hdd_score_reference PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")
target_compile_features(test_hdd_score_reference PRIVATE c_std_11)
target_compile_options(test_hdd_score_reference PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME rufus_hdd_score_reference COMMAND test_hdd_score_reference)
set_tests_properties(rufus_hdd_score_reference PROPERTIES LABELS "component;no-hardware" TIMEOUT 10)
