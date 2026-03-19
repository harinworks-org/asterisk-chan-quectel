cmake_minimum_required(VERSION 3.15)
project(asterisk-chan-quectel LANGUAGES C)

include(cmake/GenerateVersion.cmake)
include(cmake/AsteriskVersion.cmake)

option(ICONV_CONST "iconv() const declaration of input buffer" OFF)
option(PTIME_USE_DEFAULT "Use default framing" ON)
option(WITH_APPLICATIONS "Build applications" ON)
option(WITH_MSG_TECH "Build with message handler" ON)

set(ICONV_T iconv_t CACHE STRING "iconv type")
set(AST_MODULE chan_quectel)

set(CMAKE_REQUIRED_DEFINITIONS "-DAST_MODULE_SELF_SYM=__internal_chan_quectel_self")

add_library(AsteriskModule INTERFACE)
target_compile_definitions(AsteriskModule INTERFACE "AST_MODULE_SELF_SYM=__internal_chan_quectel_self")

set(AST_SCAN_PATH ${CMAKE_SOURCE_DIR})
if(AST_HEADER_DIR)
    set(AST_SCAN_PATH ${AST_HEADER_DIR})
endif()

scan_asterisk_headers(AST_SCAN ${AST_SCAN_PATH})

if(AST_SCAN_RESULT STREQUAL "DIRECTORY")
    if(AST_HEADER_DIR)
        set(CMAKE_REQUIRED_INCLUDES ${AST_SCAN_DIRECTORY})
        target_include_directories(AsteriskModule BEFORE INTERFACE ${AST_SCAN_DIRECTORY})
    endif()
elseif(AST_SCAN_RESULT STREQUAL "ARCHIVE")
    set(AST_HEADERS_ARCHIVE ${AST_SCAN_ARCHIVE})
endif()

if(AST_HEADERS_ARCHIVE)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf ${AST_HEADERS_ARCHIVE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/include)
    set(CMAKE_REQUIRED_INCLUDES ${CMAKE_BINARY_DIR}/include)
    target_include_directories(AsteriskModule BEFORE INTERFACE ${CMAKE_BINARY_DIR}/include)
endif()

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Iconv REQUIRED)

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

configure_file(config.h.in include/config.h)
configure_file(src/ptime-config.h.in include/ptime-config.h)

include(GNUInstallDirs)
add_subdirectory(src)

find_program(DPKG_EXEC dpkg)
if(DPKG_EXEC)
    execute_process(
        COMMAND ${DPKG_EXEC} --print-architecture
        OUTPUT_VARIABLE CPACK_PACKAGE_ARCHITECTURE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

set(CPACK_PACKAGE_VERSION ${VERSION_STRING})
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}_${VERSION_FILE_STRING}_${CPACK_PACKAGE_ARCHITECTURE}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${CMAKE_PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_CONTACT "me@harin.net")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Harin Lee")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md)
set(CPACK_SET_DESTDIR ON)
set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR DEB)

set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${CPACK_PACKAGE_ARCHITECTURE})
set(CPACK_DEBIAN_FILE_NAME "${CMAKE_PROJECT_NAME}_${VERSION_FILE_STRING}_${CPACK_PACKAGE_ARCHITECTURE}.deb")
set(CPACK_DEBIAN_PACKAGE_SECTION comm)
set(CPACK_DEBIAN_PACKAGE_DEPENDS asterisk)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "usb-modeswitch")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

file(COPY cpack/deb DESTINATION cpack
    FILE_PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ
    PATTERN "postinst" PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${PROJECT_BINARY_DIR}/cpack/deb/conffiles
    ${PROJECT_BINARY_DIR}/cpack/deb/postinst
)

include(CPack)
