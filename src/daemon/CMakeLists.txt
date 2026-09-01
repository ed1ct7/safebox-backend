# safeboxd. с SAFEBOX_WEB_DIST фронт вшивается в exe (cmake/EmbedWeb.cmake)

add_executable(safeboxd
    main.cpp
    config.cpp
    web_assets.cpp
)

# http (наружное) + infra (внутреннее). Порядок не важен, важно что других
# зависимостей нет: daemon ничего не знает о domain/application напрямую -
# они приезжают транзитивно через safebox-core внутри safebox-http.
target_link_libraries(safeboxd PRIVATE safebox-http safebox-infra)

sbx_apply_warnings(safeboxd)

if(SAFEBOX_WEB_DIST)
    include(${CMAKE_SOURCE_DIR}/cmake/EmbedWeb.cmake)
    sbx_embed_web(safeboxd "${SAFEBOX_WEB_DIST}")
endif()
