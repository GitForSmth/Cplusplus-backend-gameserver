cmake_minimum_required(VERSION 3.11)

project(game_server CXX)
set(CMAKE_CXX_STANDARD 20)

include(${CMAKE_BINARY_DIR}/conanbuildinfo_multi.cmake)
conan_basic_setup(TARGETS)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(game_server
    src/main.cpp
    src/database.cpp
)

target_include_directories(game_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(game_server PRIVATE
    CONAN_PKG::libpqxx
    CONAN_PKG::libpq
    CONAN_PKG::boost
    CONAN_PKG::nlohmann_json
    Threads::Threads
    -static-libstdc++
    -static-libgcc
)
