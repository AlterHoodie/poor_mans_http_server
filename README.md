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

**Not tested on AWS.** Nitro/ENA virtualized networking on EC2 does not provide a straightforward host bridge for TAP traffic — after enough fighting with AWS's networking model, this path was abandoned in favor of comparing `l4_impl` and `dpdk` on AWS only.

**Local (home server):** all three implementations (including this one) land in roughly the same ~8k–15k req/s ballpark locally. The bottleneck is the test rig, not the code. This branch is additionally penalized architecturally: every packet crosses the kernel boundary twice (NIC → kernel network stack → TAP fd read → userspace) before the hand-rolled TCP stack ever sees it.

## Flamegraph

Profiled with `perf` + flamegraph. See [`flamegraph.svg`](flamegraph.svg) for the full picture.

**Local c1000 (HTTP):**

- **`epoll_wait` (~45%)** — waiting for the next frame on the TAP fd.
- **`pump_connection` (<2%)**, **`parse_request` (<1%)** — HTTP parsing barely registers; most time is I/O wait or userspace TCP bookkeeping before bytes reach the shared HTTP code.

**Local UDP:** profile collapses toward the I/O path — read a frame off the TAP fd and walk it through hand-rolled Ethernet and IP parsing, rather than asking the kernel's UDP stack for a datagram.

## Bottlenecks

The two code-level improvements that would help most under load:

**Socket creation/destruction (~9.9% of CPU)**

Right now every TCP connection allocates a fresh `TCPSocket` and destroys it when the connection closes. The `TCPSocket` struct holds a `std::queue<int>` for its accept queue, and `std::queue` is backed by `std::deque` internally. Deque teardown is surprisingly expensive because it frees memory in chunks — so even though the queue is usually empty by the time we destroy the socket, we're still paying that cost on every single connection close. The fix is a socket pool: pre-allocate a bunch of `TCPSocket`s at startup, hand them out on accept, and return them to the pool on close instead of freeing.

**Connection map erase + string allocations (~6.3% + misc)**

Active connections are tracked in an `std::unordered_map<int, HTTPConnection>`. Calling `.erase()` on every connection close shows up at ~6.3% — not huge on its own but it adds up under load. More annoying is the HTTP layer: `parse_request` and `build_response_string` both do a bunch of `std::string` construction and copying on every request. Since this is a short-lived server handling tiny responses, switching to `string_view` for parsing (zero-copy reads over the existing buffer) and a pre-built static response string for the common case would cut most of that overhead.
