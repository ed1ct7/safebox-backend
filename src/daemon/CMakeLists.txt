# safeboxd. с SAFEBOX_WEB_DIST фронт вшивается в exe (cmake/EmbedWeb.cmake)

add_executable(safeboxd
    main.cpp
    config.cpp
    web_assets.cpp
)

# http (наружное) + infra (внутреннее). других зависимостей нет: domain и
# application приезжают транзитивно через safebox-core внутри safebox-http.
target_link_libraries(safeboxd PRIVATE safebox-http safebox-infra)

target_compile_definitions(safeboxd PRIVATE SAFEBOX_VERSION="${PROJECT_VERSION}")

sbx_apply_warnings(safeboxd)
sbx_apply_sanitizers(safeboxd)
sbx_package_static(safeboxd)

if(SAFEBOX_WEB_DIST)
    include(${PROJECT_SOURCE_DIR}/cmake/EmbedWeb.cmake)
    sbx_embed_web(safeboxd "${SAFEBOX_WEB_DIST}")
endif()
