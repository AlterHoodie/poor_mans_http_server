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

```
wrk -t4 -c100 -d30s http://192.168.29.12:80/hi
```

Below were the throuput metrics:
```
Running 30s test @ http://192.168.29.12:80/hi
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   619.12ms  484.15ms   1.99s    83.74%
    Req/Sec    38.35     34.10   180.00     76.56%
  1790 requests in 30.07s, 110.13KB read
  Socket errors: connect 0, read 0, write 0, timeout 480
Requests/sec: 59.52
```


## Bottlenecks

Profiled with `perf` + flamegraph. See [`flamegraph.svg`](flamegraph.svg) for the full picture. The two things that hurt the most:

**Socket creation/destruction (~9.9% of CPU)**

Right now every TCP connection allocates a fresh `TCPSocket` and destroys it when the connection closes. The `TCPSocket` struct holds a `std::queue<int>` for its accept queue, and `std::queue` is backed by `std::deque` internally. Deque teardown is surprisingly expensive because it frees memory in chunks — so even though the queue is usually empty by the time we destroy the socket, we're still paying that cost on every single connection close. The fix is a socket pool: pre-allocate a bunch of `TCPSocket`s at startup, hand them out on accept, and return them to the pool on close instead of freeing.

**Connection map erase + string allocations (~6.3% + misc)**

Active connections are tracked in an `std::unordered_map<int, HTTPConnection>`. Calling `.erase()` on every connection close shows up at ~6.3% — not huge on its own but it adds up under load. More annoying is the HTTP layer: `parse_request` and `build_response_string` both do a bunch of `std::string` construction and copying on every request. Since this is a short-lived server handling tiny responses, switching to `string_view` for parsing (zero-copy reads over the existing buffer) and a pre-built static response string for the common case would cut most of that overhead.
