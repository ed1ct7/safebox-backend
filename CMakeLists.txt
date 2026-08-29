# safebox v2 backend
# слои: domain <- application <- http, infra реализует порты домена, daemon собирает все вместе.
# схема слоев в docs/architecture.md

cmake_minimum_required(VERSION 3.24)
project(safebox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # нет молчательного отката на более старый стандарт
set(CMAKE_CXX_EXTENSIONS OFF)         # чистый -std=c++23, без gnu++-расширений

# глобальные переключатели: до add_subdirectory
option(SAFEBOX_BUILD_TESTS "Build Catch2 tests" OFF)   # catch2 за фичей vcpkg "tests"
set(SAFEBOX_WEB_DIST "" CACHE PATH "frontend/dist to embed into safeboxd")  # пусто = только API

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# ядро: домен + применение, ноль сторонних библиотек
add_library(safebox-core INTERFACE)

# слои по правилу зависимостей: только внутрь
add_subdirectory(src/domain)          # INTERFACE, header-only, никого не знает
add_subdirectory(src/application)     # STATIC, зависит только от domain
target_link_libraries(safebox-core INTERFACE safebox-domain safebox-application)
add_subdirectory(src/infrastructure)  # STATIC, domain + сторонние либы PRIVATE
add_subdirectory(src/http)            # STATIC, safebox-core + httplib + nlohmann-json
add_subdirectory(src/daemon)          # EXECUTABLE, http + infra

if(SAFEBOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
