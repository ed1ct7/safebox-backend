# статическая сборка safeboxd.exe (триплет x64-windows-static).
# рантайм /MT надо выставить глобально до создания таргетов, иначе LNK2038 на RuntimeLibrary

if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(SAFEBOX_STATIC_RUNTIME ON)
else()
    set(SAFEBOX_STATIC_RUNTIME OFF)
endif()

function(sbx_package_static target)
    if(SAFEBOX_STATIC_RUNTIME)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
