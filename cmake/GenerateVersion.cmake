find_program(GIT_EXEC git)

if(GIT_EXEC)
    execute_process(
        COMMAND ${GIT_EXEC} describe --dirty --match "v*" --long --tags
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT GIT_DESCRIBE)
    set(GIT_DESCRIBE "v1.0.0-0-g0000000")
endif()

string(
    REGEX MATCH "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$"
    _is_match "${GIT_DESCRIBE}"
)

if(_is_match)
    set(CHAN_VER_MAJOR ${CMAKE_MATCH_1})
    set(CHAN_VER_MINOR ${CMAKE_MATCH_2})
    set(CHAN_VER_PATCH ${CMAKE_MATCH_3})
    set(CHAN_VER_TWEAK ${CMAKE_MATCH_4})
    set(CHAN_VER_DIRTY ${CMAKE_MATCH_6})
else()
    set(CHAN_VER_MAJOR 1)
    set(CHAN_VER_MINOR 0)
    set(CHAN_VER_PATCH 0)
    set(CHAN_VER_TWEAK 0)
    set(CHAN_VER_DIRTY "")
endif()

if(CHAN_VER_TWEAK EQUAL 0 AND NOT CHAN_VER_DIRTY)
    set(VERSION_STRING "${CHAN_VER_MAJOR}.${CHAN_VER_MINOR}.${CHAN_VER_PATCH}")
else()
    set(VERSION_STRING "${CHAN_VER_MAJOR}.${CHAN_VER_MINOR}.${CHAN_VER_PATCH}-${CHAN_VER_TWEAK}${CHAN_VER_DIRTY}")
endif()

set(VERSION_FILE_STRING "${CHAN_VER_MAJOR}.${CHAN_VER_MINOR}.${CHAN_VER_PATCH}_${CHAN_VER_TWEAK}${CHAN_VER_DIRTY}")
