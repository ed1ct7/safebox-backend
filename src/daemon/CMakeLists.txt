# safeboxd. с SAFEBOX_WEB_DIST фронт вшивается в exe (cmake/EmbedWeb.cmake)

if(WIN32)
    enable_language(RC)   # иконка exe; project() объявлен только с CXX
endif()

add_executable(safeboxd
    main.cpp
    config.cpp
    web_assets.cpp
)

# http (наружное) + infra (внутреннее). domain и application приезжают транзитивно
# через safebox-core внутри safebox-http.
target_link_libraries(safeboxd PRIVATE safebox-http safebox-infra)

if(WIN32)
    # Окно приложения (UF-19): WebView2 из vcpkg. В x64-windows-static загрузчик
    # линкуется статически (один exe), в x64-windows - WebView2Loader.dll рядом с exe.
    find_package(unofficial-webview2 CONFIG REQUIRED)
    target_sources(safeboxd PRIVATE app_window.cpp single_instance.cpp safeboxd.rc)
    target_link_libraries(safeboxd PRIVATE unofficial::webview2::webview2 dwmapi)
    # подсистема WINDOWS (нет консоли при двойном щелчке), но main остается точкой входа
    set_target_properties(safeboxd PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(safeboxd PRIVATE /ENTRY:mainCRTStartup)
    endif()
    set_source_files_properties(safeboxd.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/safebox.ico")
endif()

target_compile_definitions(safeboxd PRIVATE SAFEBOX_VERSION="${PROJECT_VERSION}")

sbx_apply_warnings(safeboxd)
sbx_apply_sanitizers(safeboxd)
sbx_package_static(safeboxd)

if(SAFEBOX_WEB_DIST)
    include(${PROJECT_SOURCE_DIR}/cmake/EmbedWeb.cmake)
    sbx_embed_web(safeboxd "${SAFEBOX_WEB_DIST}")
endif()
