# тесты (-DSAFEBOX_BUILD_TESTS=ON, catch2 ставится фичей vcpkg "tests")
#   application - на InMemoryVaultStore, но с настоящим libsodium
#   http        - на фейковых сервисах, без сокетов
#   infra       - настоящие sqlite/sodium/stb/miniz
#   integration - весь стек на случайном порту

find_package(Catch2 3 REQUIRED)   # доступен только с фичей vcpkg "tests"

add_library(safebox-test-fakes INTERFACE)
target_include_directories(safebox-test-fakes INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes"
)
# фейки реализуют доменные порты -> им нужны интерфейсы domain
target_link_libraries(safebox-test-fakes INTERFACE safebox-domain)

# application: интеракторы
add_executable(safebox-app-tests
    application/safe_service_test.cpp
    application/idle_policy_test.cpp
    application/import_export_test.cpp
)
target_link_libraries(safebox-app-tests PRIVATE
    safebox-core      # тестируемые сервисы
    safebox-infra     # настоящий SodiumCrypto (см. шапку файла)
    safebox-test-fakes
    Catch2::Catch2
)
add_test(NAME application COMMAND safebox-app-tests)

# http: транспорт на фейковых сервисах
add_executable(safebox-http-tests
    http/api_test.cpp
)
target_link_libraries(safebox-http-tests PRIVATE
    safebox-http
    safebox-test-fakes
    Catch2::Catch2
)
add_test(NAME http COMMAND safebox-http-tests)

# infra: адаптеры против реальных sqlite/libsodium
add_executable(safebox-infra-tests
    infra/sqlite_store_test.cpp
)
target_link_libraries(safebox-infra-tests PRIVATE
    safebox-infra
    Catch2::Catch2
)
add_test(NAME infra COMMAND safebox-infra-tests)

# integration: safeboxd, ephemeral порт, весь UF-флоу
add_executable(safebox-integration-tests
    integration/api_test.cpp
)
target_link_libraries(safebox-integration-tests PRIVATE
    safebox-http
    safebox-infra
    Catch2::Catch2
)
add_test(NAME integration COMMAND safebox-integration-tests)
