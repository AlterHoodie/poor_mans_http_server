cmake_minimum_required(VERSION 3.20)

project(poor_mans_http_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
# If this fails, install libdpdk-dev (Debian/Ubuntu) or dpdk-devel (RHEL),
# or point PKG_CONFIG_PATH at your DPDK install's lib/pkgconfig.
pkg_check_modules(LIBDPDK REQUIRED IMPORTED_TARGET libdpdk)

add_executable(stack
src/core/event_loop.cpp
src/core/dpdk.cpp
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
  target_compile_options(stack PRIVATE -Wall -Wextra -Wpedantic -Werror -mssse3)
endif()
target_include_directories(stack PRIVATE include)
target_link_libraries(stack PRIVATE PkgConfig::LIBDPDK)
