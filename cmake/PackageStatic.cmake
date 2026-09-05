# статическая сборка safeboxd.exe (триплет x64-windows-static).
# рантайм /MT надо выставить глобально до создания таргетов, иначе LNK2038 на RuntimeLibrary

function(sbx_package_static target)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
