# safebox-domain: header-only, без зависимостей. модель, правила, порты

add_library(safebox-domain INTERFACE)

target_include_directories(safebox-domain INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# std::expected - C++23; требование едет ко всем потребителям домена.
target_compile_features(safebox-domain INTERFACE cxx_std_23)
