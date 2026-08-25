cmake_minimum_required(VERSION 3.24)
project(safebox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)

add_subdirectory(src/daemon)
add_subdirectory(src/application)
add_subdirectory(src/infrastructure)
add_subdirectory(src/domain)
