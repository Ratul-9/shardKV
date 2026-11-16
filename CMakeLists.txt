cmake_minimum_required(VERSION 3.25)
project(lldstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One sanitizer per build tree: ASan and TSan cannot be combined, so each gets a preset.
set(LLDSTORE_SANITIZER "none" CACHE STRING "Sanitizer: none | address | undefined | thread")
set_property(CACHE LLDSTORE_SANITIZER PROPERTY STRINGS none address undefined thread)
option(LLDSTORE_WERROR "Treat warnings in our own code as errors" ON)

# Sanitizer flags are global so third-party code (gtest, benchmark) is instrumented too.
# TSan in particular needs every translation unit instrumented to track happens-before.
if(LLDSTORE_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
elseif(LLDSTORE_SANITIZER STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(LLDSTORE_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT LLDSTORE_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown LLDSTORE_SANITIZER '${LLDSTORE_SANITIZER}'")
endif()

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)
FetchContent_MakeAvailable(googletest benchmark)

# Warnings apply only to our code, never to fetched dependencies.
add_library(lldstore_warnings INTERFACE)
target_compile_options(lldstore_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(LLDSTORE_WERROR)
  target_compile_options(lldstore_warnings INTERFACE -Werror)
endif()

find_package(Threads REQUIRED)

add_library(lldstore_common INTERFACE)
target_include_directories(lldstore_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lldstore_common INTERFACE lldstore_warnings Threads::Threads)

# Compiled code for Phases 7-11 (WAL, Raft, networking, metrics). Header-only parts stay in
# lldstore_common.
add_library(lldstore_core STATIC
  src/log_file.cpp
  src/wal.cpp
  src/durable_store.cpp
  src/raft.cpp
  src/linearizability.cpp
  src/net.cpp
  src/kv_node.cpp
  src/kv_client.cpp)
target_link_libraries(lldstore_core PUBLIC lldstore_common)

enable_testing()
add_subdirectory(proto)
add_subdirectory(tests)
add_subdirectory(bench)
add_subdirectory(apps)
