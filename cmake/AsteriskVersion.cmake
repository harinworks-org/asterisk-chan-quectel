find_path(_ast_dir NAMES asterisk/autoconfig.h)

if(_ast_dir)
    file(READ "${_ast_dir}/asterisk/autoconfig.h" _ast_content)
    unset(_ast_dir CACHE)

    if(_ast_content MATCHES "#define[ \t]+PACKAGE_VERSION[ \t]+\"([0-9]+)\"")
        math(EXPR ASTERISK_VERSION_NUM "${CMAKE_MATCH_1} * 10000")
    endif()
endif()
