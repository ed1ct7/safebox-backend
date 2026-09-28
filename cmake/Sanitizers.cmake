# sbx_apply_sanitizers(<target>), список берется из SAFEBOX_SANITIZERS ("address", "undefined").
# UBSan у MSVC нет. ASan на MSVC не работает с /RTC и инкрементальной линковкой,
# поэтому пресет msvc-asan собирается в RelWithDebInfo

set(SAFEBOX_SANITIZERS "" CACHE STRING "Comma-separated sanitizers: address, undefined")

function(sbx_apply_sanitizers target)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR NOT SAFEBOX_SANITIZERS)
        return()
    endif()

    string(REPLACE "," ";" _san_list "${SAFEBOX_SANITIZERS}")

    if(MSVC)
        if("address" IN_LIST _san_list)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            # Статические либы vcpkg (Catch2 и др.) собраны без ASan: аннотации
            # std::vector/std::string иначе дают LNK2038/LNK1319 "mismatch detected".
            target_compile_definitions(${target} PRIVATE
                _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
            if(_type STREQUAL "EXECUTABLE")
                target_link_options(${target} PRIVATE /INCREMENTAL:NO)
            endif()
        endif()
        if("undefined" IN_LIST _san_list)
            message(WARNING "UBSan недоступен в MSVC - используйте пресет clang-ubsan")
        endif()
    else()
        set(_flags)
        if("address" IN_LIST _san_list)
            list(APPEND _flags -fsanitize=address)
        endif()
        if("undefined" IN_LIST _san_list)
            list(APPEND _flags -fsanitize=undefined -fno-sanitize-recover=undefined)
        endif()
        target_compile_options(${target} PRIVATE ${_flags} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE ${_flags})
    endif()
endfunction()
