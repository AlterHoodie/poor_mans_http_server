cmake_minimum_required(VERSION 3.20)

project(poor_mans_http_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(stack
src/core/event_loop.cpp
src/core/tap.cpp
src/core/buff.cpp
src/core/utils.cpp
src/link/eth.cpp
src/link/arp.cpp
src/net/ip.cpp
src/net/icmp.cpp
src/transport/tcp.cpp
src/main.cpp
src/router/router.cpp
src/http/parser.cpp
)

if(MSVC)
  target_compile_options(stack PRIVATE /W4 /WX)
else()
  target_compile_options(stack PRIVATE -Wall -Wextra -Wpedantic -Werror -g -O2 -fno-omit-frame-pointer)
endif()
target_include_directories(stack PRIVATE include)
