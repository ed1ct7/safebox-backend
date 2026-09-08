# safebox-http: зависит от core + httplib + nlohmann-json. infra отсюда не видно

find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(safebox-http STATIC
    src/server.cpp
    src/router.cpp
    src/guard_mw.cpp
    src/auth_mw.cpp
    src/errors.cpp
    src/json.cpp
    src/safe_api.cpp
    src/entries_api.cpp
    src/import_api.cpp
    src/media_api.cpp
    src/search_api.cpp
)

target_include_directories(safebox-http
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"  # daemon читает server.hpp
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"      # api.hpp, dto.hpp - только для .cpp слоя
)

# safebox-core PUBLIC: server.hpp ссылается на типы сервисов application.
# httplib/json PRIVATE: наружу слой отдает свой HttpServer, а не httplib.
target_link_libraries(safebox-http
    PUBLIC  safebox-core
    PRIVATE httplib::httplib nlohmann_json::nlohmann_json
)

sbx_apply_warnings(safebox-http)
sbx_apply_sanitizers(safebox-http)
