#
# asterisk-chan-quectel
#

CMAKE_MINIMUM_REQUIRED(VERSION 3.20)

FUNCTION(SetProjectVersion cmajor cminor cpatch ctweak cstatus)
    MATH(EXPR MMAJOR "${cmajor}")
    MATH(EXPR MMINOR "${cminor}")
    MATH(EXPR MPATCH "${cpatch}")
    MATH(EXPR MTWEAK "${ctweak}")
    IF(${cstatus} MATCHES "dirty$")
        SET(IS_DIRTY 1)
    ELSE()
        UNSET(IS_DIRTY)
    ENDIF()

    IF(${MTWEAK} LESS_EQUAL 0 AND NOT IS_DIRTY)
        # short form
        SET(CHAN_VER_STR "${MMAJOR}.${MMINOR}.${MPATCH}" PARENT_SCOPE)
        SET(CHAN_VER_FSTR "${MMAJOR}.${MMINOR}.${MPATCH}" PARENT_SCOPE)
    ELSE()
        # long form
        SET(CHAN_VER_STR "${MMAJOR}.${MMINOR}.${MPATCH}-${ctweak}${cstatus}" PARENT_SCOPE)
        SET(CHAN_VER_FSTR "${MMAJOR}.${MMINOR}.${MPATCH}_${ctweak}${cstatus}" PARENT_SCOPE)
    ENDIF()
ENDFUNCTION()

INCLUDE(cmake/asterisk-headers.cmake)
INCLUDE(cmake/asterisk-version-num.cmake)
INCLUDE(cmake/clang-format.cmake)

#
#
# CHAN_SOURCE_VERSION = git hash
# CHAN_SOURCE_TS = timestamp of ${CHAN_SOURCE_VERSION}
#
# CHAN_VER_MAJOR CHAN_VER_MINOR CHAN_VER_PATCH CHAN_VER_TWEAK
#
# CHAN_STATUS
#

UNSET(OPENWRT_PACKAGE)
UNSET(CHAN_SOURCE_VERSION)
UNSET(CHAN_SOURCE_TS)
UNSET(CHAN_STATUS)

IF(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/.git)
    SET(IS_GIT_REPO "TRUE")
    FIND_PROGRAM(GIT_EXECUTABLE git REQUIRED)
    EXECUTE_PROCESS(
        COMMAND ${GIT_EXECUTABLE} describe --abbrev=6 --dirty --match "v*" --long --tags
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        RESULT_VARIABLE GIT_ERROR_CODE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    IF(NOT GIT_ERROR_CODE)
        STRING(REGEX MATCH "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+)(-.+))?$" PROGRAM_VERSION_MATCH ${GIT_DESCRIBE})
        SET(CHAN_VER_MAJOR ${CMAKE_MATCH_1})
        SET(CHAN_VER_MINOR ${CMAKE_MATCH_2})
        SET(CHAN_VER_PATCH ${CMAKE_MATCH_3})
        SET(CHAN_VER_TWEAK ${CMAKE_MATCH_5})
        set(CHAN_STATUS ${CMAKE_MATCH_6})
        SET(OPENWRT_PACKAGE 1)
    ELSE()
        UNSET(CHAN_STATUS)
        MESSAGE(FATAL_ERROR "Git - unable to describe")
    ENDIF()

    EXECUTE_PROCESS(
        COMMAND ${GIT_EXECUTABLE} log -n 1 "--pretty=format:%H;%at"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_LOG
        RESULT_VARIABLE GIT_ERROR_CODE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    IF(NOT GIT_ERROR_CODE)
        LIST(GET GIT_LOG 0 CHAN_SOURCE_VERSION)
        LIST(GET GIT_LOG 1 CHAN_SOURCE_TS)
    ELSE()
        MESSAGE(FATAL_ERROR "Git - unable to get timestamp of last commit")
    ENDIF()
ELSE()
    UNSET(IS_GIT_REPO)
    IF(EXISTS ${CMAKE_SOURCE_DIR}/pkg-vars.cmake)
        INCLUDE(${CMAKE_SOURCE_DIR}/pkg-vars.cmake)
    ENDIF()
ENDIF()

#
# clang-format executable
# expected version: 18
#
IF(IS_GIT_REPO)
    SET(CLANG_FORMAT CLANG_FORMAT-NOTFOUND CACHE FILEPATH "C/C++/Java/JavaScript/JSON/Objective-C/Protobuf/C# code formatter")
    ClangFormatFindAndCheck(18)
ENDIF()

IF(NOT(DEFINED CHAN_VER_MAJOR AND DEFINED CHAN_VER_MINOR AND DEFINED CHAN_VER_PATCH AND DEFINED CHAN_VER_TWEAK))
    STRING(TIMESTAMP CHAN_VER_MAJOR "%Y" UTC)
    STRING(TIMESTAMP CHAN_VER_MINOR "%m" UTC)
    STRING(TIMESTAMP CHAN_VER_PATCH "%d" UTC)
    STRING(TIMESTAMP CHAN_VER_TWEAK "%H%M" UTC)    
    IF(DEFINED ENV{SOURCE_DATE_EPOCH})
        UNSET(CHAN_STATUS)
        STRING(TIMESTAMP CHAN_SOURCE_TS "%s" UTC)
        STRING(TIMESTAMP CHAN_SOURCE_VERSION "%s" UTC)
    ELSE()
        MESSAGE(WARNING "Project version taken from current timestamp")
        SET(CHAN_STATUS "-unversioned")
        SET(CHAN_SOURCE_VERSION "NOTFOUND")
        SET(CHAN_SOURCE_TS 0)
    ENDIF()
ENDIF()

SetProjectVersion("${CHAN_VER_MAJOR}" "${CHAN_VER_MINOR}" "${CHAN_VER_PATCH}" "${CHAN_VER_TWEAK}" "${CHAN_STATUS}")
MESSAGE(DEBUG "Source version: ${CHAN_SOURCE_VERSION} [${CHAN_SOURCE_TS}]")
MESSAGE(STATUS "Project version: ${CHAN_VER_STR}")

# CMake
INCLUDE(CMakeDependentOption)
OPTION(CHECK_SOURCE_DATE_EPOCH "Check existience of SOURCE_DATE_EPOCH environment variable" OFF)
OPTION(BUILD_CHAN_QUECTEL_BINARY "Build chan_quectel binary" ON)
OPTION(BUILD_OPENWRT_MAKEFILE "Build OpenWRT Makefile" OFF)
SET(AST_HEADER_DIR CACHE PATH "Location of Asterisk headers")
SET(ASTERISK_VERSION_NUM NOTFOUND CACHE STRING "Asterisk version")
OPTION(LINKER_VERBOSE "Linker - be verbose (--verbose option)" OFF)
OPTION(LINKER_TRACE "Linker - trace used objects and liraries (--trace option)" OFF)

# BUILD_CHAN_QUECTEL_BINARY
cmake_dependent_option(ICONV_CONST "iconv() const declaration of input buffer" OFF "BUILD_CHAN_QUECTEL_BINARY" OFF)
cmake_dependent_option(PTIME_USE_DEFAULT "Use default framing" ON "BUILD_CHAN_QUECTEL_BINARY" OFF)

cmake_dependent_option(WITH_APPLICATIONS "Build applications" ON "BUILD_CHAN_QUECTEL_BINARY" OFF)
cmake_dependent_option(WITH_MSG_TECH "Build with message handler" ON "BUILD_CHAN_QUECTEL_BINARY" OFF)

# BUILD_OPENWRT_MAKEFILE
cmake_dependent_option(BUILD_OPENWRT_MAKEFILE_STANDALONE "Build standalone OpenWRT Makefile" ON "BUILD_OPENWRT_MAKEFILE;OPENWRT_PACKAGE" OFF)

IF(CHECK_SOURCE_DATE_EPOCH)
    ADD_SUBDIRECTORY(cmake/source-date-epoch source-date-epoch)
ENDIF()

PROJECT(asterisk-chan-quectel
    VERSION ${CHAN_VER_MAJOR}.${CHAN_VER_MINOR}.${CHAN_VER_PATCH}.${CHAN_VER_TWEAK}
    HOMEPAGE_URL "http://github.com/harinworks-org/asterisk-chan-quectel"
    DESCRIPTION "Asterisk channel driver for Quectel modules"
    LANGUAGES NONE
)

IF(BUILD_CHAN_QUECTEL_BINARY)
    ENABLE_LANGUAGE(C)
    IF(IS_GIT_REPO)
        INCLUDE(CTest)
    ENDIF()

    CheckAsteriskVersion()

    SET(ICONV_T iconv_t CACHE STRING "iconv type")
    SET(AST_MODULE chan_quectel)

    INCLUDE(CheckPIESupported)
    INCLUDE(CheckIncludeFile)
    INCLUDE(CheckIncludeFiles)
    INCLUDE(CheckSymbolExists)

    SET(CMAKE_REQUIRED_DEFINITIONS "-DAST_MODULE_SELF_SYM=__internal_chan_quectel_self")

    CHECK_PIE_SUPPORTED(OUTPUT_VARIABLE PIE_DETAILS LANGUAGES C)
    IF(NOT CMAKE_C_LINK_PIE_SUPPORTED)
        MESSAGE(WARNING "PIE is not supported at link time: ${PIE_DETAILS}")
    ENDIF()

    ADD_LIBRARY(AsteriskModule INTERFACE)
    TARGET_COMPILE_DEFINITIONS(AsteriskModule INTERFACE "AST_MODULE_SELF_SYM=__internal_chan_quectel_self")
    IF(AST_HEADER_DIR)
        ScanAsteriskHeadersDirectory(AST_SCAN ${AST_HEADER_DIR})
        IF(NOT AST_SCAN_RESULT)
            MESSAGE(FATAL_ERROR "Invalid value of AST_HEADER_DIR: ${AST_HEADER_DIR} - ${AST_SCAN_ERROR_MSG}")
        ENDIF()
        IF(${AST_SCAN_RESULT} STREQUAL "DIRECTORY")
            MESSAGE(DEBUG "Asterisk header directory: ${AST_SCAN_DIRECTORY}")
            SET(CMAKE_REQUIRED_INCLUDES ${AST_SCAN_DIRECTORY})
            TARGET_INCLUDE_DIRECTORIES(AsteriskModule BEFORE INTERFACE ${AST_SCAN_DIRECTORY})
        ELSEIF(${AST_SCAN_RESULT} STREQUAL "ARCHIVE")
            MESSAGE(STATUS "Importing asterisk headers from ${AST_SCAN_ARCHIVE}")
            FILE(ARCHIVE_EXTRACT INPUT "${AST_SCAN_ARCHIVE}" DESTINATION "${CMAKE_BINARY_DIR}/include")
            SET(CMAKE_REQUIRED_INCLUDES "${CMAKE_BINARY_DIR}/include")
            TARGET_INCLUDE_DIRECTORIES(AsteriskModule BEFORE INTERFACE "${CMAKE_BINARY_DIR}/include")
        ELSEIF(${AST_SCAN_RESULT} STREQUAL "NONE")
            MESSAGE(FATAL_ERROR "Invalid value of AST_HEADER_DIR: ${AST_HEADER_DIR} - could not find asterisk headers")
        ENDIF()
    ELSE()
        ScanAsteriskHeadersDirectory(AST_SCAN ${CMAKE_SOURCE_DIR})
        IF(NOT AST_SCAN_RESULT)
            MESSAGE(WARNING "Looking for Asterisk headers archive: ${AST_SCAN_ERROR_MSG}")
        ELSEIF(${AST_SCAN_RESULT} STREQUAL "ARCHIVE")
            MESSAGE(STATUS "Importing asterisk headers from ${AST_SCAN_ARCHIVE}")
            FILE(ARCHIVE_EXTRACT INPUT "${AST_SCAN_ARCHIVE}" DESTINATION "${CMAKE_BINARY_DIR}/include")
            SET(CMAKE_REQUIRED_INCLUDES "${CMAKE_BINARY_DIR}/include")
            TARGET_INCLUDE_DIRECTORIES(AsteriskModule BEFORE INTERFACE "${CMAKE_BINARY_DIR}/include")
        ELSEIF(NOT ${AST_SCAN_RESULT} STREQUAL "NONE")
            MESSAGE(WARNING "Looking for Asterisk headers archive: unexpected result - ${AST_SCAN_RESULT}")
        ENDIF()
    ENDIF()

    CHECK_INCLUDE_FILE(asterisk.h HAVE_ASTERISK_H)
    IF(NOT HAVE_ASTERISK_H)
        MESSAGE(FATAL_ERROR "Asterisk header not found.")
    ENDIF()

    IF(DEFINED CMAKE_REQUIRED_INCLUDES)
        FIND_PATH(AST_HEADERS_SUBDIR NAMES buildopts.h PATHS ${CMAKE_REQUIRED_INCLUDES} PATH_SUFFIXES asterisk NO_CACHE NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    ELSE()
        FIND_PATH(AST_HEADERS_SUBDIR NAMES buildopts.h PATH_SUFFIXES asterisk NO_CACHE REQUIRED)
    ENDIF()
    IF(IS_GIT_REPO)
        ShowAstBuildOptSum(${AST_HEADERS_SUBDIR})
    ENDIF()

    CHECK_INCLUDE_FILES("stdarg.h;asterisk.h;asterisk/frame.h" HAVE_ASTERISK_FRAME_H)
    CHECK_SYMBOL_EXISTS(AST_CONTROL_SRCCHANGE "stdarg.h;asterisk.h;asterisk/frame.h" HAVE_AST_CONTROL_SRCCHANGE)

    CHECK_INCLUDE_FILE(fcntl.h HAVE_FCNTL_H)
    CHECK_INCLUDE_FILE(inttypes.h HAVE_INTTYPES_H)

    CHECK_INCLUDE_FILE(memory.h HAVE_MEMORY_H)
    CHECK_SYMBOL_EXISTS(memchr memory.h HAVE_MEMCHR)
    CHECK_SYMBOL_EXISTS(memmove memory.h HAVE_MEMMOVE)
    CHECK_SYMBOL_EXISTS(memset memory.h HAVE_MEMSET)

    CHECK_INCLUDE_FILE(stdint.h HAVE_STDINT_H)

    CHECK_INCLUDE_FILE(stdlib.h HAVE_STDLIB_H)
    CHECK_SYMBOL_EXISTS(realpath stdlib.h HAVE_REALPATH)
    CHECK_SYMBOL_EXISTS(strtol stdlib.h HAVE_STRTOL)

    CHECK_INCLUDE_FILE(string.h HAVE_STRING_H)
    CHECK_SYMBOL_EXISTS(memmem string.h HAVE_MEMMEM)

    CHECK_INCLUDE_FILE(strings.h HAVE_STRINGS_H)
    CHECK_SYMBOL_EXISTS(strcasecmp strings.h HAVE_STRCASECMP)
    CHECK_SYMBOL_EXISTS(strncasecmp strings.h HAVE_STRNCASECMP)

    CHECK_INCLUDE_FILE(sys/stat.h HAVE_SYS_STAT_H)
    CHECK_INCLUDE_FILE(sys/time.h HAVE_SYS_TIME_H)
    CHECK_INCLUDE_FILE(sys/types.h HAVE_SYS_TYPES_H)
    CHECK_INCLUDE_FILE(termios.h HAVE_TERMIOS_H)
    CHECK_INCLUDE_FILE(unistd.h HAVE_UNISTD_H)

    FIND_PACKAGE(Threads REQUIRED)
    FIND_PACKAGE(ALSA 1.1.2 REQUIRED)
    FIND_PACKAGE(SQLite3 3.6.5 REQUIRED)
    FIND_PACKAGE(Iconv REQUIRED)

    IF(ICONV_CONST)
        SET(ICONV_CONST_STR "const")
    ELSE()
        SET(ICONV_CONST_STR "/**/")
    ENDIF()

    IF(HAVE_AST_CONTROL_SRCCHANGE)
        SET(HAVE_AST_CONTROL_SRCCHANGE_STR 1)
    ELSE()
        SET(HAVE_AST_CONTROL_SRCCHANGE_STR "/**/")
    ENDIF()

    CONFIGURE_FILE(config.h.in include/config.h)
    CONFIGURE_FILE(src/ptime-config.h.in include/ptime-config.h)

    INCLUDE(GNUInstallDirs)
    ADD_SUBDIRECTORY(src)

#
# CPack
# Based on: http://decovar.dev/blog/2021/09/23/cmake-cpack-package-deb-apt/
#
    IF(IS_GIT_REPO)
        IF(NOT DEFINED CPACK_PACKAGE_ARCHITECTURE)
            FIND_PROGRAM(DPKG_EXECUTABLE dpkg)
            IF(NOT DPKG_EXECUTABLE)
                MESSAGE(WARNING "Can not find dpkg in your path, setting CPACK_PACKAGE_ARCHITECTURE to amd64.")
                SET(CPACK_PACKAGE_ARCHITECTURE amd64)
            ELSE()
                EXECUTE_PROCESS(
                    COMMAND "${DPKG_EXECUTABLE}" --print-architecture
                    OUTPUT_VARIABLE CPACK_PACKAGE_ARCHITECTURE
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            ENDIF()
        ENDIF()

        SET(CPACK_PACKAGE_VERSION ${CHAN_VER_STR})
        SET(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}_${CHAN_VER_FSTR}_${CPACK_PACKAGE_ARCHITECTURE}")
        SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${CMAKE_PROJECT_DESCRIPTION}")
        SET(CPACK_PACKAGE_DESCRIPTION "Asterisk channel driver for Quectel and SimCOM modules.")
        IF(NOT DEFINED CPACK_PACKAGE_CONTACT)
            SET(CPACK_PACKAGE_CONTACT "roed@onet.eu")
        ENDIF()
        SET(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md")
        SET(CPACK_SET_DESTDIR ON)
        SET(CPACK_STRIP_FILES ON)
        SET(CPACK_THREADS 0)
        SET(CPACK_ARCHIVE_THREADS 0)
        LIST(APPEND CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/cpack/post-build-deb-info.cmake")
        LIST(APPEND CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/cpack/post-build-lintian.cmake")

        IF(NOT DEFINED CPACK_GENERATOR)
            SET(CPACK_GENERATOR TGZ DEB)
        ENDIF()
        SET(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${CPACK_PACKAGE_ARCHITECTURE})
        SET(CPACK_DEBIAN_FILE_NAME "${CMAKE_PROJECT_NAME}_${CHAN_VER_FSTR}_${CPACK_PACKAGE_ARCHITECTURE}.deb")
        IF(CPACK_PACKAGE_MAINTAINER)
            SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_MAINTAINER} <${CPACK_PACKAGE_CONTACT}>")
        ELSE()
            SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_VENDOR} <${CPACK_PACKAGE_CONTACT}>")
        ENDIF()
        SET(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
        SET(CPACK_DEBIAN_PACKAGE_SECTION comm)
        SET(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
        SET(CPACK_DEBIAN_PACKAGE_RECOMMENDS "usb-modeswitch")
        IF(${ASTERISK_VERSION_NUM} LESS 170000)
            SET(CPACK_DEBIAN_PACKAGE_DEPENDS asterisk16)
            SET(CPACK_DEBIAN_PACKAGE_ENHANCES asterisk16)
        ELSE()
            SET(CPACK_DEBIAN_PACKAGE_DEPENDS asterisk)
            SET(CPACK_DEBIAN_PACKAGE_ENHANCES asterisk)
        ENDIF()
        SET(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")

        FILE(COPY cpack/deb DESTINATION cpack
            FILE_PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ
            PATTERN "postinst" PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
        )
        SET(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${PROJECT_BINARY_DIR}/cpack/deb/conffiles ${PROJECT_BINARY_DIR}/cpack/deb/postinst)

        IF(NOT DEFINED CPACK_SOURCE_GENERATOR)
            SET(CPACK_SOURCE_GENERATOR TGZ)
        ENDIF()
        SET(CPACK_SOURCE_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}_${CHAN_VER_FSTR}_src")
        SET(CPACK_SOURCE_IGNORE_FILES
            "/build/"
            "/install/"
            "/package/"
            "/test/"
            "/tools/"
            "/\.vscode/"
            "/\.git/"
            "/\.gitignore"
            "/\.gitattributes"
        )

        INCLUDE(CPack)
    ENDIF()
ENDIF() # BUILD_CHAN_QUECTEL_BINARY

IF(BUILD_OPENWRT_MAKEFILE)
    ADD_SUBDIRECTORY(openwrt)
ENDIF()
