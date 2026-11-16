cmake_minimum_required(VERSION 3.20)
project(redisCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# All server logic: RESP, keyspace, commands, event-loop plumbing.
add_library(redis_core STATIC
  src/resp.cpp
  src/context.cpp
  src/server.cpp
  src/commands/registry.cpp
  src/commands/strings.cpp
  src/commands/lists.cpp
  src/commands/streams.cpp
)
target_include_directories(redis_core PUBLIC src)
target_compile_options(redis_core PRIVATE -Wall -Wextra)

# The server (epoll) and the select() variant used for benchmarking.
add_executable(redis src/main_epoll.cpp)
target_link_libraries(redis PRIVATE redis_core)

add_executable(redis-select src/main_select.cpp)
target_link_libraries(redis-select PRIVATE redis_core)

# Tests
option(REDISCPP_BUILD_TESTS "Build tests" ON)
if(REDISCPP_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  FetchContent_MakeAvailable(googletest)

  add_executable(unit_tests tests/resp_test.cpp)
  target_link_libraries(unit_tests PRIVATE redis_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME e2e_epoll
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/e2e_test.py $<TARGET_FILE:redis> 7101)
  add_test(NAME e2e_select
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/e2e_test.py $<TARGET_FILE:redis-select> 7102)
endif()
