file(GLOB SRC_FILES *.c)
file(GLOB HDR_FILES *.h)

add_library(asterisk-chan-quectel SHARED ${SRC_FILES} ${HDR_FILES})
target_compile_features(asterisk-chan-quectel PUBLIC c_std_99)

target_include_directories(asterisk-chan-quectel BEFORE PRIVATE ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(asterisk-chan-quectel PRIVATE
    _GNU_SOURCE
    HAVE_CONFIG_H
)

target_link_libraries(asterisk-chan-quectel PRIVATE
    AsteriskModule
    Threads::Threads
    ALSA::ALSA
    SQLite::SQLite3
    Iconv::Iconv
)

set_target_properties(asterisk-chan-quectel PROPERTIES
    LIBRARY_OUTPUT_NAME ${AST_MODULE}
    PREFIX ""
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(asterisk-chan-quectel PRIVATE
    $<$<AND:$<C_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,8>>:-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=bin -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=src>
    $<$<AND:$<C_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,4>>:-Wall>
)

if(CMAKE_LIBRARY_ARCHITECTURE)
    install(TARGETS asterisk-chan-quectel DESTINATION lib/${CMAKE_LIBRARY_ARCHITECTURE}/asterisk/modules)
else()
    install(TARGETS asterisk-chan-quectel DESTINATION ${CMAKE_INSTALL_LIBDIR}/asterisk/modules)
endif()

install(FILES ${CMAKE_SOURCE_DIR}/quectel.conf DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/asterisk)
