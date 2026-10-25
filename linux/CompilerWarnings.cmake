# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)

option(RUFUS_WARNINGS_AS_ERRORS "Fail native builds on compiler warnings" ON)
if(RUFUS_WARNINGS_AS_ERRORS)
    foreach(language IN ITEMS C CXX)
        if("${CMAKE_${language}_COMPILER_ID}" MATCHES "^(GNU|Clang|AppleClang)$")
            add_compile_options("$<$<COMPILE_LANGUAGE:${language}>:-Werror>")
        endif()
    endforeach()
endif()
