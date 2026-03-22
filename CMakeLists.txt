cmake_minimum_required(VERSION 3.15)
project(asterisk-chan-quectel LANGUAGES C)

option(ICONV_CONST "iconv() const declaration of input buffer" OFF)
option(PTIME_USE_DEFAULT "Use default framing" ON)
option(WITH_APPLICATIONS "Build applications" ON)
option(WITH_MSG_TECH "Build with message handler" ON)

include(cmake/GenerateVersion.cmake)
include(cmake/AsteriskVersion.cmake)
include(cmake/Packaging.cmake)

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Iconv REQUIRED)

set(PROJECT_VERSION ${CHAN_VER_MAJOR}.${CHAN_VER_MINOR}.${CHAN_VER_PATCH}.${CHAN_VER_TWEAK})
set(PROJECT_VERSION_MAJOR ${CHAN_VER_MAJOR})
set(PROJECT_VERSION_MINOR ${CHAN_VER_MINOR})
set(PROJECT_VERSION_PATCH ${CHAN_VER_PATCH})
set(PROJECT_VERSION_TWEAK ${CHAN_VER_TWEAK})
set(PROJECT_HOMEPAGE_URL "https://github.com/harinworks/asterisk-chan-quectel")
set(PROJECT_DESCRIPTION "Asterisk channel driver for Quectel modules")

if(ICONV_CONST)
    set(ICONV_CONST_STR "const")
else()
    set(ICONV_CONST_STR "/**/")
endif()

if(HAVE_AST_CONTROL_SRCCHANGE)
    set(HAVE_AST_CONTROL_SRCCHANGE_STR 1)
else()
    set(HAVE_AST_CONTROL_SRCCHANGE_STR "/**/")
endif()

set(ICONV_T iconv_t CACHE STRING "iconv type")
set(ASTERISK_VERSION_NUM "" CACHE STRING "Asterisk version number")
set(AST_MODULE chan_quectel)

configure_file(cmake/config.h.in
    ${CMAKE_BINARY_DIR}/generated/config.h
)

configure_file(cmake/ptime-config.h.in
    ${CMAKE_BINARY_DIR}/generated/ptime-config.h
)

add_library(AsteriskModule INTERFACE)

target_compile_definitions(AsteriskModule INTERFACE
    "AST_MODULE_SELF_SYM=__internal_chan_quectel_self"
)

add_subdirectory(src)
