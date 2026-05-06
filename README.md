# poor_mans_http_server

A minimal **HTTP/1.1-style TCP server** written in C++17 for Linux. It is deliberately small: one process, **non-blocking sockets**, an **epoll** event loop, a tiny HTTP parser, and a simple method/path router. The goal is to show how a production-style server *conceptually* handles many connections without one thread per client—using the kernel’s readiness notifications instead of blocking on each socket.

**What it does today**

- Listens on **port 8080** (`0.0.0.0`).
- `**GET /`** → `200 OK` with body `Hello World\n`.
- `**POST /**` → `200 OK` echoing the request body.
- Parses request headers and optional body using `Content-Length`.
- Supports **keep-alive** when the client does not send `Connection: close`; otherwise closes after the response.

---

## Build and run

```bash
make          # produces ./server
make run      # build (if needed) and run
```

Requires a Linux environment with epoll (e.g. native Linux or WSL2). The code uses POSIX APIs: `epoll`, `timerfd`, `fcntl` for non-blocking I/O.

---

## Architecture (high level)


| Piece                 | Role                                                                                     |
| --------------------- | ---------------------------------------------------------------------------------------- |
| `src/main.cpp`        | Epoll loop, accepts clients, reads/writes, drives connection state machine, idle timeout |
| `src/net/socket.cpp`  | `set_non_blocking`, `epoll_ctl` helpers (`add_epoll_event`, `modify_epoll_event`)        |
| `src/net/socket.h`    | `Connection` state (`READING_HEADERS` → … → `WRITING`) and buffers                       |
| `src/http/parser.*`   | Turn raw bytes into `Request`, build response string                                     |
| `src/router/router.*` | Map `(HttpMethod, path)` → handler function                                              |


Connections are stored in `std::unordered_map<int, Connection>` keyed by client file descriptor.

---

## What is epoll?

**epoll** is a Linux-specific facility for **I/O multiplexing**: you register many file descriptors (sockets, pipes, etc.) with a single **epoll instance**, then **block once** in `epoll_wait` until *something* becomes readable, writable, or signals an error—depending on which events you asked for.

Compared to older patterns:

- `**select` / `poll`** scale poorly when the number of fds grows (the kernel must scan large fd sets each call).
- **epoll** is designed for high fan-in: adding an fd and asking “wake me when this socket is readable” is cheap, and `epoll_wait` returns only **ready** fds.

Conceptually you get: **one thread (or a few) can serve thousands of connections** because work is driven by **readiness**: you only read/write when the kernel says the operation won’t block *immediately* (for non-blocking sockets, combined with draining until `EAGAIN`).

**Important details**

- **Level-triggered (default)** vs **edge-triggered** (`EPOLLET`): this project uses default **level-triggered** behavior. If data is still sitting in the socket buffer, epoll will keep reporting readability until you drain it (which matches the “read until `EAGAIN`” loop in the code).
- epoll is **Linux-only**; other Unix systems use `kqueue` (BSD/macOS), `IOCP` (Windows), etc.

---

## PS

This repo was created to learn various systems engineering, profiling and Cpp concepts and hence proper attention wasnt given to code quality or optimizations.

- **Single thread**: Simple and correct for learning; a real server might use a thread pool, `SO_REUSEPORT`, or separate accept/worker processes.
- **Parse/route in the event thread**: Long handlers block everyone; production systems often offload work.
- **No TLS, HTTP/2, chunked encoding, etc.** — intentionally out of scope for “poor man’s” server.

## Profiling Observations

The profiling flamegraph referenced in the following observations is available in [flamegraph.svg](flamegraph.svg). Please consult it for a visual breakdown of CPU usage during benchmarking of the server. Hot paths are highlighted and help correlate code structure to runtime costs.

Benchmark performed using:

```bash
wrk -t4 -c100 -d30s http://18.232.66.57:8080/
```

### `accept` (~14%)

High `accept()` overhead due to large amounts of short-lived TCP connection churn.
Each connection requires:

- socket allocation
- TCP handshake management
- fd creation
- scheduler coordination

Can be reduced by increasing keepalive timeout and reusing connections at the cost of higher memory/fd retention. 

Basically shifts the cost from CPU to Memory, kind of like a design decision one has to make when one is building a server for a particular problem statement.

---

### `epoll_ctl` (~4.23%)

Mostly a skill issue / naive implementation issue.

The current implementation repeatedly switches epoll interest states:

```text
accept -> read (EPOLLOUT) -> epoll_ctl -> write (EPOLLIN)
```

This creates unnecessary syscall overhead due to repeated kernel epoll metadata updates.

Can be optimized by:

- registering sockets once with:

```cpp
EPOLLIN | EPOLLOUT
```

- maintaining read/write state in userspace instead of kernel space. Of course more application/user level code implementations and state management.

---

### `epoll_wait` (~26.76%)

Expected behavior for event-driven architectures.

Most of the time is spent:

- blocked waiting for socket readiness events
- sleeping/waking through scheduler coordination

This is not active CPU spinning, but scheduler/syscall orchestration overhead.

---

### `pump_connection` (~9.86%)

Mainly caused by:

- repeated string operations
- `substr`
- `append`
- request parsing
- unnecessary buffer copying

Can be improved using:

- pointer/offset based parsing
- ring buffers
- fewer allocations/copies
- better buffer ownership models

Another skill issue moment.

---

### `unordered_map<fd, Connection>` (~18.31%)

The connection table sits directly in the event-loop hot path.

Although hash maps provide average `O(1)` lookups, they:

- require hashing
- involve pointer chasing
- hurt cache locality

Since file descriptors are already integer indexes, a contiguous array/vector indexed by fd would likely perform much better.

---

### `write` (~8.45%)

The overhead here was mostly:

- syscall transitions
- kernel TCP stack processing
- userspace -> kernel buffer copying
- packetization overhead

rather than actual network bandwidth saturation.

Small-request workloads tend to become syscall/TCP-stack bound rather than throughput bound.

---

## Biggest Takeaway

The majority of runtime was spent in:

- scheduler activity
- syscall handling
- TCP stack coordination
- memory movement

rather than actual HTTP parsing or application logic, this is due to the nature of the test - being high throughput short-lived connections.



This type of workload is common in high-frequency trading (HFT) systems where applications may process millions of exchange messages every second. At this scale, even small delays caused by syscalls and repeated switching between userspace and kernel space become expensive.

To reduce this overhead, some trading systems use technologies like DPDK which allow applications to talk directly to the network card (NIC), bypassing much of the Linux kernel networking stack. This helps reduce latency and improves performance consistency.

Frameworks like nginx usually do not use this approach because their workloads are dominated by things like:

- TLS encryption

- HTTP parsing

- reverse proxying

- load balancing

- compression

In these cases, the main bottleneck is often application-level processing rather than raw packet handling, so the normal Linux networking stack together with epoll is usually more than fast enough.

---

## Quick manual test

```bash
# terminal 1
./server

# terminal 2
curl -v http://127.0.0.1:8080/
curl -v -d 'payload' http://127.0.0.1:8080/
```

---

## Summary

**poor_mans_http_server** demonstrates a **Linux epoll-driven**, **non-blocking** HTTP server: one `epoll_wait` loop multiplexes the listening socket, a periodic **timerfd** for idle cleanup, and all client connections—with **EPOLLIN** while reading requests and **EPOLLOUT** while writing responses, switching interest with `**epoll_ctl`** as each connection moves through its small state machine.
