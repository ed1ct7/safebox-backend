# sbx_apply_sanitizers(<target>), список берется из SAFEBOX_SANITIZERS ("address", "undefined").
# UBSan у MSVC нет. ASan на MSVC не работает с /RTC и инкрементальной линковкой,
# поэтому пресет msvc-asan собирается в RelWithDebInfo

set(SAFEBOX_SANITIZERS "" CACHE STRING "Comma-separated sanitizers: address, undefined")

function(sbx_apply_sanitizers target)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    if(NOT SAFEBOX_SANITIZERS)
        return()
    endif()

    # разбор "address,undefined" -> список
    string(REPLACE "," ";" _san_list "${SAFEBOX_SANITIZERS}")

    if(MSVC)
        if("address" IN_LIST _san_list)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
        # UBSan на MSVC не существует - см. шапку файла
    else()
        set(_flags)
        if("address" IN_LIST _san_list)
            list(APPEND _flags -fsanitize=address)
        endif()
        if("undefined" IN_LIST _san_list)
            list(APPEND _flags -fsanitize=undefined)
        endif()
        list(APPEND _flags -fno-omit-frame-pointer)
        target_compile_options(${target} PRIVATE ${_flags})
    endif()
endfunction()
