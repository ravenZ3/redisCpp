cmake_minimum_required(VERSION 3.13)

project(redis-starter-cpp)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

set(CMAKE_CXX_STANDARD 23)
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)

add_executable(redis ${SOURCE_FILES})

target_include_directories(redis PRIVATE /usr/include)
target_link_libraries(redis PRIVATE Threads::Threads)