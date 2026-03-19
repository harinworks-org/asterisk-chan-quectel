find_program(GIT_EXEC git)

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    set(SOURCE_DIR "${CMAKE_SOURCE_DIR}")
endif()

if(GIT_EXEC)
    execute_process(
        COMMAND ${GIT_EXEC} describe --abbrev=6 --dirty --match "v*" --long --tags
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    execute_process(
        COMMAND ${GIT_EXEC} rev-parse --verify --short HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT GIT_DESCRIBE)
    set(GIT_DESCRIBE "v1.0.0-0-unknown")
endif()

if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

string(REGEX MATCH "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$"
    _match ${GIT_DESCRIBE})

if(_match)
    set(VER_MAJOR ${CMAKE_MATCH_1})
    set(VER_MINOR ${CMAKE_MATCH_2})
    set(VER_PATCH ${CMAKE_MATCH_3})
    set(VER_TWEAK ${CMAKE_MATCH_4})
    set(VER_DIRTY ${CMAKE_MATCH_6})

    if(VER_TWEAK EQUAL 0 AND NOT VER_DIRTY)
        set(VERSION_STRING "${VER_MAJOR}.${VER_MINOR}.${VER_PATCH}")
    else()
        set(VERSION_STRING "${VER_MAJOR}.${VER_MINOR}.${VER_PATCH}-${VER_TWEAK}${VER_DIRTY}")
    endif()

    set(VERSION_FILE_STRING "${VER_MAJOR}.${VER_MINOR}.${VER_PATCH}_${VER_TWEAK}${VER_DIRTY}")
else()
    set(VERSION_STRING "1.0.0-1")
    set(VERSION_FILE_STRING "1.0.0_1")
endif()
