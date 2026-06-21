# poor_mans_http_server

A userspace HTTP server backed by my barely working TCP/IP stack, built on top of a Linux TAP device. The stack starts at Layer 2 — it reads raw Ethernet frames directly from `tap0` and implements everything up the chain itself: ARP, IPv4, ICMP, TCP, and a minimal HTTP layer. Everything below Ethernet (the physical link, drivers, kernel networking) is handled transparently by the kernel through the TAP interface.

The server runs a single-threaded `epoll` event loop. Incoming Ethernet frames are dispatched through a protocol handler chain, and TCP connections are surfaced via a BSD-socket-like API (`tcp_socket`, `tcp_bind`, `tcp_listen`, `tcp_accept`, `tcp_recv`, `tcp_send`).

## Stack

```
TAP device (tap0)
       |
   Ethernet
   /       \
 ARP        IP
           /  \
        ICMP   TCP
                |
              HTTP
```

## Test Setup

Tested on a home server running Linux. To give the TAP interface access to the outside network, a bridge (`br0`) was created with both `tap0` and the physical NIC (`eth0`) attached to it. The server binds to `192.168.29.12:80`.

```sh
ip link add br0 type bridge
ip link set eth0 master br0
ip link set tap0 master br0
ip link set br0 up
```

## Benchmark

> **Note:** A CPU-intensive operation was added per request so the benchmark exercises per-request processing cost rather than just busy-wait NIC polling (the NIC fills the RX ring slower than the CPU can drain it on this hardware).

### Standard

```
wrk -t8 -c200 -d30s http://192.168.29.12/hi --timeout 10s
Running 30s test @ http://192.168.29.12/hi
  8 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    14.39ms   92.36ms   1.56s    98.01%
    Req/Sec     1.73k     1.39k    5.27k    54.69%
  351870 requests in 28.98s, 22.82MB read
  Socket errors: connect 0, read 0, write 0, timeout 46
Requests/sec:  12139.76

wrk -t8 -c1000 -d30s http://192.168.29.12/hi --timeout 10s
Running 30s test @ http://192.168.29.12/hi
  8 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    16.66ms  115.28ms   2.09s    98.61%
    Req/Sec     2.18k     2.19k    9.39k    75.77%
  418214 requests in 28.96s, 27.12MB read
  Socket errors: connect 0, read 0, write 0, timeout 81
Requests/sec:  14439.90
```

### Pipelined

```
wrk -t4 -c100 -d30s --latency -s pipeline.lua http://192.168.29.12/hi
Running 30s test @ http://192.168.29.12/hi
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    25.77ms   98.87ms 990.90ms   96.43%
    Req/Sec    13.06k     8.46k   35.71k    46.16%
  Latency Distribution
     50%    7.97ms
     75%   11.31ms
     90%   15.51ms
     99%  645.87ms
  1141206 requests in 28.95s, 74.01MB read
  Socket errors: connect 0, read 0, write 0, timeout 14
Requests/sec:  39416.58
```


## Bottlenecks

Profiled with `perf` + flamegraph. See [`flamegraph.svg`](flamegraph.svg) for the full picture. The two things that hurt the most:

**Socket creation/destruction (~9.9% of CPU)**

Right now every TCP connection allocates a fresh `TCPSocket` and destroys it when the connection closes. The `TCPSocket` struct holds a `std::queue<int>` for its accept queue, and `std::queue` is backed by `std::deque` internally. Deque teardown is surprisingly expensive because it frees memory in chunks — so even though the queue is usually empty by the time we destroy the socket, we're still paying that cost on every single connection close. The fix is a socket pool: pre-allocate a bunch of `TCPSocket`s at startup, hand them out on accept, and return them to the pool on close instead of freeing.

**Connection map erase + string allocations (~6.3% + misc)**

Active connections are tracked in an `std::unordered_map<int, HTTPConnection>`. Calling `.erase()` on every connection close shows up at ~6.3% — not huge on its own but it adds up under load. More annoying is the HTTP layer: `parse_request` and `build_response_string` both do a bunch of `std::string` construction and copying on every request. Since this is a short-lived server handling tiny responses, switching to `string_view` for parsing (zero-copy reads over the existing buffer) and a pre-built static response string for the common case would cut most of that overhead.
