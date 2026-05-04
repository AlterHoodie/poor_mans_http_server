# poor_mans_http_server

A minimal **HTTP/1.1-style TCP server** written in C++17 for Linux. It is deliberately small: one process, **non-blocking sockets**, an **epoll** event loop, a tiny HTTP parser, and a simple method/path router. The goal is to show how a production-style server *conceptually* handles many connections without one thread per client—using the kernel’s readiness notifications instead of blocking on each socket.

**What it does today**

- Listens on **port 8080** (`0.0.0.0`).
- **`GET /`** → `200 OK` with body `Hello World\n`.
- **`POST /`** → `200 OK` echoing the request body.
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

| Piece | Role |
|--------|------|
| `src/main.cpp` | Epoll loop, accepts clients, reads/writes, drives connection state machine, idle timeout |
| `src/net/socket.cpp` | `set_non_blocking`, `epoll_ctl` helpers (`add_epoll_event`, `modify_epoll_event`) |
| `src/net/socket.h` | `Connection` state (`READING_HEADERS` → … → `WRITING`) and buffers |
| `src/http/parser.*` | Turn raw bytes into `Request`, build response string |
| `src/router/router.*` | Map `(HttpMethod, path)` → handler function |

Connections are stored in `std::unordered_map<int, Connection>` keyed by client file descriptor.

---

## What is epoll?

**epoll** is a Linux-specific facility for **I/O multiplexing**: you register many file descriptors (sockets, pipes, etc.) with a single **epoll instance**, then **block once** in `epoll_wait` until *something* becomes readable, writable, or signals an error—depending on which events you asked for.

Compared to older patterns:

- **`select` / `poll`** scale poorly when the number of fds grows (the kernel must scan large fd sets each call).
- **epoll** is designed for high fan-in: adding an fd and asking “wake me when this socket is readable” is cheap, and `epoll_wait` returns only **ready** fds.

Conceptually you get: **one thread (or a few) can serve thousands of connections** because work is driven by **readiness**: you only read/write when the kernel says the operation won’t block *immediately* (for non-blocking sockets, combined with draining until `EAGAIN`).

**Important details**

- **Level-triggered (default)** vs **edge-triggered** (`EPOLLET`): this project uses default **level-triggered** behavior. If data is still sitting in the socket buffer, epoll will keep reporting readability until you drain it (which matches the “read until `EAGAIN`” loop in the code).
- epoll is **Linux-only**; other Unix systems use `kqueue` (BSD/macOS), `IOCP` (Windows), etc.

---

## How this server uses epoll

### 1. Create the epoll instance

`epoll_create1(0)` returns `epfd`, a handle for **`epoll_ctl`** (register/modify/remove fds) and **`epoll_wait`** (wait for events).

### 2. Register interest in three kinds of fds

1. **Listening socket** (`server_fd`) — `EPOLLIN`: new clients may be acceptable.
2. **Timer fd** (`tfd`) — `EPOLLIN`: periodic tick for idle timeouts (see below).
3. **Each client socket** — initially `EPOLLIN` only; when a full request is parsed and a response is queued, the code switches that fd to **`EPOLLOUT`** so epoll notifies when the socket can accept more write data without blocking.

Helper wrappers live in `src/net/socket.cpp`: `add_epoll_event` (`EPOLL_CTL_ADD`) and `modify_epoll_event` (`EPOLL_CTL_MOD`).

### 3. The main loop: `epoll_wait`

The server runs an infinite loop:

```text
epoll_wait(epfd, events, 1024, -1)  // wait until at least one event (no timeout)
for each event:
  dispatch by fd
```

`-1` means **no time limit** on the wait itself; the program does not use `epoll_wait`’s millisecond timeout for HTTP idle handling. Idle handling is separate (timerfd).

### 4. Accepting clients (listening socket ready)

When `events[i].data.fd == server_fd` and `EPOLLIN` fires, the code **`accept`s in a loop**. Because the listen socket is **non-blocking**, `accept` returns `EAGAIN`/`EWOULDBLOCK` when there is no pending connection—then the inner loop stops. That drains the backlog of ready accepts in one epoll wakeup.

New client fds are set non-blocking and registered with **`EPOLLIN`**. Each gets a `Connection` entry with state starting at `READING_HEADERS`.

### 5. Reading requests (`EPOLLIN` on a client)

On client `EPOLLIN`, the server **reads in a tight loop** into a stack buffer and appends to `conn.read_buf`. It stops when:

- `read` returns **0** (peer closed) → close and remove connection.
- `read` returns **-1** and `errno` is `EAGAIN` or `EWOULDBLOCK` → kernel buffer drained for now; wait for the next epoll wakeup.
- other errors → close and remove.

Then `pump_connection` tries to parse: find `\r\n\r\n`, parse headers, optionally wait for body bytes per `Content-Length`, build a `Request`, call `router.route`, serialize the response into `write_buf`, clear `read_buf`, set state to **writing**, and **`modify_epoll_event(epfd, EPOLLOUT, fd)`** so the next readiness is for sending.

### 6. Writing responses (`EPOLLOUT`)

When `EPOLLOUT` fires, the server **writes from `write_buf`** until empty or until `write` hits `EAGAIN`. If the buffer empties:

- If **keep-alive**: reset read/write buffers and parser-related fields, set state back to **reading headers**, refresh `last_active`, and **`modify_epoll_event` back to `EPOLLIN`**.
- If not keep-alive: **`close`** the fd and erase the connection.

Between “request fully parsed” and “response fully sent”, the connection only waits for **write** readiness—so epoll is not spamming `EPOLLIN` on that fd while you’re trying to push the response.

---

## Timeouts: not `epoll_wait`, but `timerfd`

HTTP **idle timeout** here is **10 seconds** (`TIMEOUT` in `src/main.cpp`). It is **not** implemented with `epoll_wait(..., timeout_ms)` because that would tie “wake up” to a single global timer and complicate mixing socket events with a different policy.

Instead:

1. **`timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)`** creates a fd that becomes **readable** on a schedule.
2. **`timerfd_settime`** arms it to fire **every 1 second** (interval and initial value both 1s).
3. That timer fd is **`EPOLLIN` registered on the same `epfd`**.

When `epoll_wait` returns with `fd == tfd`, the code **`read`s 8 bytes** (the expiration count) to clear the readable state, takes `steady_clock::now()`, and iterates all connections: if **`now - conn.last_active` > 10 seconds**, it **`close`s** the socket and removes the entry.

**What counts as “active”**

- New connections initialize `last_active` at construction time.
- After a **full response** is written on a **keep-alive** connection, `last_active` is updated when resetting for the next request.

So the timeout is an **idle** policy: time since the last completed response cycle (for keep-alive) or time since accept if the client never finishes a request—aligned with “no progress for 10s.”

---

## Design trade-offs (educational)

- **Single thread**: Simple and correct for learning; a real server might use a thread pool, `SO_REUSEPORT`, or separate accept/worker processes.
- **Parse/route in the event thread**: Long handlers block everyone; production systems often offload work.
- **No TLS, HTTP/2, chunked encoding, etc.** — intentionally out of scope for “poor man’s” server.

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

**poor_mans_http_server** demonstrates a **Linux epoll-driven**, **non-blocking** HTTP server: one `epoll_wait` loop multiplexes the listening socket, a periodic **timerfd** for idle cleanup, and all client connections—with **EPOLLIN** while reading requests and **EPOLLOUT** while writing responses, switching interest with **`epoll_ctl`** as each connection moves through its small state machine.
