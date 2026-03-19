function(scan_asterisk_headers VAR_PREFIX AST_DIR)
    if(NOT IS_DIRECTORY "${AST_DIR}")
        return()
    endif()

    if(EXISTS "${AST_DIR}/asterisk.h" AND IS_DIRECTORY "${AST_DIR}/asterisk")
        get_filename_component(_dir ${AST_DIR} REALPATH)
        set(${VAR_PREFIX}_RESULT "DIRECTORY" PARENT_SCOPE)
        set(${VAR_PREFIX}_DIRECTORY "${_dir}" PARENT_SCOPE)
        return()
    endif()

    file(GLOB _archives "${AST_DIR}/asterisk-headers.tar" "${AST_DIR}/asterisk-headers.tar.*")
    list(LENGTH _archives _len)
    if(_len EQUAL 1)
        set(${VAR_PREFIX}_RESULT "ARCHIVE" PARENT_SCOPE)
        set(${VAR_PREFIX}_ARCHIVE "${_archives}" PARENT_SCOPE)
        return()
    endif()
endfunction()

function(get_ast_buildopt_sum AST_HEADERS_SUBDIR)
    get_filename_component(AST_HEADERS_SUBDIR ${AST_HEADERS_SUBDIR}/.. ABSOLUTE)
    try_run(_run _compile ${CMAKE_BINARY_DIR}/AST_BUILDOPT_SUM
        SOURCES ${CMAKE_SOURCE_DIR}/test/asterisk/AST_BUILDOPT_SUM.c
        COMPILE_DEFINITIONS "-I${AST_HEADERS_SUBDIR}"
        RUN_OUTPUT_VARIABLE _output
    )
    if(_compile AND _run)
        string(STRIP "${_output}" AST_BUILDOPT_SUM)
        if("${AST_BUILDOPT_SUM}" MATCHES "^[0-9a-f]+$")
            set(AST_BUILDOPT_SUM ${AST_BUILDOPT_SUM} PARENT_SCOPE)
        else()
            message(SEND_ERROR "Invalid AST_BUILDOPT_SUM: ${AST_BUILDOPT_SUM}")
        endif()
    else()
        message(SEND_ERROR "Failed to determine AST_BUILDOPT_SUM")
    endif()
endfunction()
