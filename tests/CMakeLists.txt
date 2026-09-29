# тесты (-DSAFEBOX_BUILD_TESTS=ON, catch2 ставится фичей vcpkg "tests")
#   application - на InMemoryVaultStore, но с настоящим libsodium
#   http        - на фейковых сервисах, без сокетов
#   infra       - настоящие sqlite/sodium/stb/miniz
#   integration - весь стек на случайном порту

find_package(Catch2 3 CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(WebP CONFIG REQUIRED)

add_library(safebox-test-fakes INTERFACE)
target_include_directories(safebox-test-fakes INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/fakes")
# фейки реализуют доменные порты и сервисы application
target_link_libraries(safebox-test-fakes INTERFACE safebox-core)

function(sbx_add_test_executable name)
    add_executable(${name} ${ARGN})
    sbx_apply_warnings(${name})
    sbx_apply_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# application: интеракторы
sbx_add_test_executable(safebox-app-tests
    application/safe_service_test.cpp
    application/idle_policy_test.cpp
    application/import_export_test.cpp
    application/entries_search_test.cpp
    application/entries_move_test.cpp
    application/format_test.cpp
    application/catalog_test.cpp
    application/tags_test.cpp
    application/tag_filter_test.cpp
    application/rules_test.cpp
    application/link_preview_test.cpp
)
target_include_directories(safebox-app-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/application/src" # white-box: IdlePolicy
)
target_link_libraries(safebox-app-tests PRIVATE
    safebox-core      # тестируемые сервисы
    safebox-infra     # настоящий SodiumCrypto (см. шапку файла)
    safebox-test-fakes
    Catch2::Catch2WithMain
)

# http: транспорт на фейковых сервисах, без сети
sbx_add_test_executable(safebox-http-tests
    http/api_test.cpp
)
target_include_directories(safebox-http-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/http/src" # white-box: registerRoutes, ApiContext
)
target_link_libraries(safebox-http-tests PRIVATE
    safebox-http
    safebox-test-fakes
    httplib::httplib
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
)

# infra: адаптеры против реальных sqlite/libsodium/stb/miniz
sbx_add_test_executable(safebox-infra-tests
    infra/sqlite_store_test.cpp
    infra/crypto_test.cpp
    infra/media_zip_test.cpp
    infra/page_fetcher_test.cpp
)
target_include_directories(safebox-infra-tests SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_include_directories(safebox-infra-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes" # store_contract.hpp: общие проверки хранилища
    "${PROJECT_SOURCE_DIR}/src/infrastructure/src" # white-box: internal_address.hpp
)
target_link_libraries(safebox-infra-tests PRIVATE
    safebox-infra
    miniz::miniz # сторонний zip-ридер для проверки StreamZipWriter
    WebP::webp   # сторонний webp-энкодер для проверки миниатюр
    Catch2::Catch2WithMain
)

# integration: весь стек на ephemeral-порту
sbx_add_test_executable(safebox-integration-tests
    integration/api_test.cpp
)
target_link_libraries(safebox-integration-tests PRIVATE
    safebox-http
    safebox-infra
    httplib::httplib
    nlohmann_json::nlohmann_json
    miniz::miniz
    Catch2::Catch2WithMain
)
