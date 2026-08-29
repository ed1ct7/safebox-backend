# safebox-application: сервисы, сессия, автоблокировка, шифрование полей.
# зависит только от domain, sqlite/sodium отсюда не видно

add_library(safebox-application STATIC
    src/vault_session.cpp
    src/safe_service.cpp
    src/idle_policy.cpp
    src/entries_service.cpp
    src/import_export.cpp
    src/search_service.cpp
    src/sealing.cpp
)

target_include_directories(safebox-application
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"   # потребители читают services.hpp
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"       # внутренние .cpp делят vault_session.hpp
)

# PUBLIC: http-слой в services.hpp видит структуры домена -> путь domain
# должен транзитивно доехать до потребителей.
target_link_libraries(safebox-application PUBLIC safebox-domain)

sbx_apply_warnings(safebox-application)
