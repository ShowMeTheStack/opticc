cmake_minimum_required(VERSION 3.25)
project(opticc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(OPTICC_BUILD_TESTS "Build unit tests" ON)
option(OPTICC_WERROR "Treat warnings as errors" OFF)

add_library(opticc_warnings INTERFACE)
target_compile_options(opticc_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(OPTICC_WERROR)
  target_compile_options(opticc_warnings INTERFACE -Werror)
endif()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_subdirectory(src/libopticc)
add_subdirectory(src/sim)
add_subdirectory(src/opticc)
add_subdirectory(src/opticcd)

if(OPTICC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

add_subdirectory(src/tools)
