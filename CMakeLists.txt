# safebox v2 backend
# слои: domain <- application <- http, infra реализует порты домена, daemon собирает все вместе.
# схема слоев в docs/architecture.md

cmake_minimum_required(VERSION 3.24)

# глобальные переключатели: до project()
# Тесты тянут catch2 через фичу vcpkg "tests": один флаг включает и то и другое
# (vcpkg читает VCPKG_MANIFEST_FEATURES во время project()).
option(SAFEBOX_BUILD_TESTS "Build Catch2 tests" OFF)
if(SAFEBOX_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
set(SAFEBOX_WEB_DIST "" CACHE PATH "frontend/dist to embed into safeboxd")  # пусто = только API

project(safebox VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # нет молчательного отката на более старый стандарт
set(CMAKE_CXX_EXTENSIONS OFF)         # чистый -std=c++23, без gnu++-расширений

if(WIN32)
    # windows.h (через cpp-httplib и в daemon) не должен ломать std::min/max.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

include(cmake/PackageStatic.cmake)    # до таргетов: рантайм /MT для статического триплета
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
