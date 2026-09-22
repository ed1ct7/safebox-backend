# тесты (-DSAFEBOX_BUILD_TESTS=ON, catch2 ставится фичей vcpkg "tests")
#   application - на InMemoryVaultStore, но с настоящим libsodium
#   http        - на фейковых сервисах, без сокетов
#   infra       - настоящие sqlite/sodium/stb/miniz
#   integration - весь стек на случайном порту

find_package(Catch2 3 CONFIG REQUIRED)

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

# infra: адаптеры против реальных sqlite/libsodium/stb/miniz
sbx_add_test_executable(safebox-infra-tests
    infra/sqlite_store_test.cpp
    infra/crypto_test.cpp
)
target_link_libraries(safebox-infra-tests PRIVATE
    safebox-infra
    Catch2::Catch2WithMain
)
