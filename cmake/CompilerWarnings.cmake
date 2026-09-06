# sbx_apply_warnings(<target>) - общие ворнинги для всех таргетов.
# /utf-8 обязателен, иначе MSVC читает русские комменты в cp1251

function(sbx_apply_warnings target)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
