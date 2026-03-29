include(GNUInstallDirs)

find_program(CLANG_FORMAT_EXEC clang-format)

set(SRC_FILES
    app.c
    at_command.c
    at_parse.c
    at_queue.c
    at_read.c
    at_response.c
    chan_quectel.c
    channel.c
    char_conv.c
    cli.c
    cpvt.c
    dc_config.c
    error.c
    eventfd.c
    helpers.c
    memmem.c
    mixbuffer.c
    monitor_thread.c
    msg_tech.c
    pcm.c
    pdu.c
    ringbuffer.c
    smsdb.c
    tty.c
)

add_library(asterisk-chan-quectel SHARED ${SRC_FILES})

target_link_libraries(asterisk-chan-quectel PRIVATE
    AsteriskModule
    Threads::Threads
    ALSA::ALSA
    SQLite::SQLite3
    Iconv::Iconv
)

target_include_directories(asterisk-chan-quectel PRIVATE
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(asterisk-chan-quectel PRIVATE
    _GNU_SOURCE
    HAVE_CONFIG_H
)

set_target_properties(asterisk-chan-quectel PROPERTIES
    LIBRARY_OUTPUT_NAME ${AST_MODULE}
    PREFIX ""
)

install(TARGETS asterisk-chan-quectel
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/asterisk/modules
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/asterisk/modules
)

install(FILES ${CMAKE_SOURCE_DIR}/quectel.conf
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/asterisk
)

if(CLANG_FORMAT_EXEC)
    file(GLOB ALL_SRC_FILES *.c *.h)

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXEC} -i ${ALL_SRC_FILES}
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXEC} --dry-run --Werror ${ALL_SRC_FILES}
    )
endif()
