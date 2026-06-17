cmake_minimum_required(VERSION 4.2)
project(poor_mans_http_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(server
    src/main.cpp
    src/http/parser.cpp
    src/net/socket.cpp
    src/router/router.cpp
)

target_include_directories(server PRIVATE src)
target_compile_options(server PRIVATE -Wall -Wextra)
target_link_libraries(server PRIVATE pthread)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(server PRIVATE -fsanitize=address)
    target_link_options(server PRIVATE -fsanitize=address)
endif()

set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer")

option(BUILD_BENCHMARKS "Build Google Benchmark targets" ON)