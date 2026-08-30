# safebox-infra: все сторонние либы (sodium, sqlite3, stb, miniz, fmt) подключены PRIVATE,
# наружу торчит только factories.hpp

add_library(safebox-infra STATIC
    src/sodium_crypto.cpp
    src/sqlite_database.cpp
    src/sqlite_schema.cpp
    src/sqlite_vault_store.cpp
    src/sqlite_blob_store.cpp
    src/stb_thumbnailer.cpp
    src/stream_zip_writer.cpp
    src/system_clock.cpp
)

target_include_directories(safebox-infra
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"  # только factories.hpp
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"      # sqlite_database.hpp и пр.
)

# PUBLIC domain: factories.hpp возвращает доменные типы (KeyHandle, VaultStore).
# PRIVATE сторонние: их заголовки видны только .cpp этого слоя.
target_link_libraries(safebox-infra
    PUBLIC safebox-domain
    PRIVATE libsodium sqlite3 stb miniz fmt
)

sbx_apply_warnings(safebox-infra)
