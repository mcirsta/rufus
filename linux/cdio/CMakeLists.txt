# SPDX-License-Identifier: GPL-3.0-or-later
# Rufus's bundled image reader, built against native Linux stdio and iconv.
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckStructHasMember)
include(CheckTypeSize)
include(CheckCSourceCompiles)
find_package(Iconv REQUIRED)
get_filename_component(RUFUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CDIO_SOURCE "${RUFUS_ROOT}/src/libcdio")
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64)

foreach(header errno.h fcntl.h inttypes.h limits.h memory.h stdarg.h stdbool.h
        stdint.h stdio.h stdlib.h strings.h string.h sys/stat.h sys/time.h
        sys/types.h unistd.h)
    string(MAKE_C_IDENTIFIER "HAVE_${header}" feature)
    string(TOUPPER "${feature}" feature)
    check_include_file("${header}" ${feature})
endforeach()
foreach(symbol fseeko ftruncate memcpy memset strdup strndup snprintf vsnprintf
        gmtime_r localtime_r timegm setenv unsetenv tzset)
    string(TOUPPER "HAVE_${symbol}" feature)
    check_symbol_exists(${symbol} "stdio.h;stdlib.h;string.h;time.h;unistd.h" ${feature})
endforeach()
check_symbol_exists(S_ISLNK "sys/stat.h" HAVE_S_ISLNK)
check_symbol_exists(S_ISSOCK "sys/stat.h" HAVE_S_ISSOCK)
check_symbol_exists(timezone "time.h" HAVE_TIMEZONE_VAR)
check_struct_has_member("struct tm" tm_gmtoff "time.h" HAVE_TM_GMTOFF)
check_struct_has_member("struct tm" tm_zone "time.h" HAVE_STRUCT_TM_TM_ZONE)
check_struct_has_member("struct timespec" tv_nsec "time.h" HAVE_STRUCT_TIMESPEC)
set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h)
check_type_size(off_t RUFUS_OFF_T_SIZE LANGUAGE C)
unset(CMAKE_EXTRA_INCLUDE_FILES)
if(NOT RUFUS_OFF_T_SIZE EQUAL 8 OR NOT HAVE_FSEEKO)
    message(FATAL_ERROR "The image reader requires 64-bit off_t and fseeko")
endif()
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    set(WORDS_BIGENDIAN 1)
elseif(NOT CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
    message(FATAL_ERROR "Cannot determine image-reader byte order")
endif()
set(HAVE_ICONV 1)
set(CMAKE_REQUIRED_LIBRARIES Iconv::Iconv)
check_c_source_compiles("
#include <iconv.h>
int main(void) {
    _Static_assert(_Generic(&iconv,
        size_t (*)(iconv_t, char**, size_t*, char**, size_t*): 1, default: 0),
        \"iconv takes mutable input pointer\");
    return 0;
}" RUFUS_ICONV_MUTABLE)
if(NOT RUFUS_ICONV_MUTABLE)
    check_c_source_compiles("
#include <iconv.h>
int main(void) {
    _Static_assert(_Generic(&iconv,
        size_t (*)(iconv_t, const char**, size_t*, char**, size_t*): 1, default: 0),
        \"iconv takes const input pointer\");
    return 0;
}" RUFUS_ICONV_CONST)
    if(NOT RUFUS_ICONV_CONST)
        message(FATAL_ERROR "Unsupported iconv signature")
    endif()
    set(ICONV_CONST const)
endif()
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_DEFINITIONS)
configure_file(config.h.in "${CMAKE_CURRENT_BINARY_DIR}/config.h")

add_library(bundled_cdio STATIC
    ../platform/cdio_image.c
    ${CDIO_SOURCE}/driver/disc.c
    ${CDIO_SOURCE}/driver/ds.c
    ${CDIO_SOURCE}/driver/logging.c
    ${CDIO_SOURCE}/driver/memory.c
    ${CDIO_SOURCE}/driver/read.c
    ${CDIO_SOURCE}/driver/sector.c
    ${CDIO_SOURCE}/driver/track.c
    ${CDIO_SOURCE}/driver/util.c
    ${CDIO_SOURCE}/driver/_cdio_stdio.c
    ${CDIO_SOURCE}/driver/_cdio_stream.c
    ${CDIO_SOURCE}/driver/utf8.c
    ${CDIO_SOURCE}/iso9660/iso9660.c
    ${CDIO_SOURCE}/iso9660/iso9660_fs.c
    ${CDIO_SOURCE}/iso9660/rock.c
    ${CDIO_SOURCE}/iso9660/xa.c
    ${CDIO_SOURCE}/udf/udf.c
    ${CDIO_SOURCE}/udf/udf_file.c
    ${CDIO_SOURCE}/udf/udf_fs.c
    ${CDIO_SOURCE}/udf/udf_time.c
    ${CDIO_SOURCE}/udf/filemode.c
)
target_compile_features(bundled_cdio PRIVATE c_std_11)
target_compile_definitions(bundled_cdio PRIVATE HAVE_CONFIG_H=1 _GNU_SOURCE)
target_compile_definitions(bundled_cdio PUBLIC DO_NOT_WANT_COMPATIBILITY _FILE_OFFSET_BITS=64)
if(WORDS_BIGENDIAN)
    target_compile_definitions(bundled_cdio PUBLIC WORDS_BIGENDIAN=1)
endif()
target_include_directories(bundled_cdio PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${CDIO_SOURCE}/driver")
target_include_directories(bundled_cdio PUBLIC "${CDIO_SOURCE}")
target_compile_options(bundled_cdio PRIVATE -Wall -Wextra)

target_link_libraries(bundled_cdio PRIVATE Iconv::Iconv)
