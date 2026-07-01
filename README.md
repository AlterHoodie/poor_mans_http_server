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

## Benchmark

Locally, on a home server with a Wi-Fi laptop as the load generator, this implementation lands in the same ~8k–15k req/s ballpark as the other branches — the NIC and client are the bottleneck, not the code.

On AWS (c6i server, c7n load generator, same VPC/AZ, `wrk -t8 -c1000 -d30s`, keep-alive):

| Metric    | Value                        |
| --------- | ---------------------------- |
| Req/sec   | ~54,000                      |
| p50       | 12–16 ms                     |
| p99       | 35 ms – 1 s+ spikes          |

Tail latency can stretch to ten times the median under load — the server depends on the kernel scheduler waking it from `epoll_wait`, kernel TCP bookkeeping, and `write()` → `tcp_sendmsg`, each a place where scheduling delay can appear.

**UDP flood (AWS):** ~700,000 packets/sec before kernel-level loss. `recvfrom` alone consumes ~92% of CPU time, with the kernel UDP stack adding more on top (softirq overlap in profiling).

---

## Profiling

See [flamegraph.svg](flamegraph.svg) for the full profile. Percentages below are from labeled runs at concurrency 1000 unless noted.

### Local c1000 (HTTP)

- **`pump_connection` (60–70%)** — header parsing, body buffering, routing, response building.
- **`parse_request` (~25%)** — re-parsing HTTP headers as text on every request.
- **`write` (13–16%)** — cost of handing bytes to the kernel via `tcp_sendmsg`.
- **`epoll_wait` (few %)** — at this concurrency the thread is almost always servicing ready connections rather than sleeping.

### Local UDP

- **`recvfrom` (~50%)**, **`epoll_wait` (~48%)** — no HTTP work; the client cannot push packets fast enough to keep the server busy.

### AWS c1000 (HTTP)

- **`pump_connection` (~72%)** — rises from ~63% locally because the faster network path delivers more requests to the application layer per second.
- **`epoll_wait`** — still negligible; this implementation is application and kernel-TX bound, not epoll-bound, at both sites.

### Improvements (still open)

- **`unordered_map<fd, Connection>`** — fd-indexed flat array would eliminate hashing and pointer chasing in the hot path.
- **`pump_connection`** — `string_view`-based parsing and fewer `substr`/`append` copies on every request.
- **`epoll_ctl`** — register sockets once with `EPOLLIN | EPOLLOUT` and track read/write state in userspace instead of toggling kernel interest per transition.

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
