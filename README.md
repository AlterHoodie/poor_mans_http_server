# poor_mans_http_server

A learning exercise exploring how an HTTP server behaves at different layers of the networking stack. Three branches implement the same `GET /hi → 200 OK` endpoint, each starting I/O at a different level — from the kernel's TCP socket API down to raw Ethernet frames read directly off the NIC.

---

## Branch Index

| Branch | I/O mechanism | Stack starts at | Req/sec (c100) | Req/sec (pipelined) |
|---|---|---|---|---|
| [`l4_impl`](../../tree/l4_impl) | Linux `epoll` + kernel TCP sockets | Layer 4 — kernel owns TCP/IP | **21,948** | 29,252 ¹ |
| [`dpdk`](../../tree/dpdk) | DPDK `rte_eth_rx_burst` — no syscalls | Layer 2 — custom ARP/IP/TCP | 15,399 | **50,753** |
| [`l2_impl`](../../tree/l2_impl) | Linux TAP device (`tap0`) | Layer 2 — custom ARP/IP/TCP | 14,440 | 39,417 |

¹ All non-2xx — `l4_impl` does not support HTTP pipelining; wrk's pipeline script sends batched requests but each is handled as a new connection.

`main` is this index only. All source code lives on the branches above.

---

## Stack Comparison

```
l4_impl                  dpdk                     l2_impl
─────────────────        ─────────────────        ─────────────────
epoll / syscalls         NIC (DPDK PMD)           TAP device (tap0)
       |                        |                        |
 kernel TCP/IP             Ethernet                 Ethernet
       |                   /       \                /       \
     HTTP                ARP        IP            ARP        IP
                                   /  \                     /  \
                                ICMP  TCP                ICMP  TCP
                                       |                        |
                                     HTTP                     HTTP
```

`l4_impl` delegates everything below HTTP to the kernel. The other two branches re-implement ARP, IPv4, ICMP, TCP, and HTTP from scratch on top of raw frames.

---

## Benchmark Comparison

### Methodology note

Earlier runs saturated the NIC RX ring faster than the CPU could drain it, so the CPU spent most of its time in a busy-wait polling loop. This masked real protocol-processing differences between the implementations. A deliberate CPU-intensive operation was added per request to shift the bottleneck off the NIC and expose per-request processing cost more clearly. Numbers below reflect that updated workload.

### Standard (no pipelining)

`wrk -t8 -c200 -d30s` and `wrk -t8 -c1000 -d30s`, `l4_impl` at `-t4 -c100` (different port/run):

| Branch | Connections | Req/sec | Avg latency | Max latency | Timeouts |
|---|---|---|---|---|---|
| `l4_impl` | 100 | **21,948** | 19.83 ms | 1.95 s | 52 |
| `dpdk` | 200 | 12,540 | 10.16 ms | 1.09 s | 23 |
| `dpdk` | 1000 | 15,399 | 9.25 ms | 2.06 s | 72 |
| `l2_impl` | 200 | 12,140 | 14.39 ms | 1.56 s | 46 |
| `l2_impl` | 1000 | 14,440 | 16.66 ms | 2.09 s | 81 |

### Pipelined (`pipeline.lua`, `wrk -t4 -c100 -d30s`)

| Branch | Req/sec | p50 latency | p99 latency | Timeouts | Note |
|---|---|---|---|---|---|
| `l4_impl` | 29,252 | — | — | 100 | All non-2xx; no pipeline support |
| `dpdk` | **50,753** | 15.67 ms | — | 64 | |
| `l2_impl` | 39,417 | 7.97 ms | 645.87 ms | 14 | |

### Why `l4_impl` wins on standard requests (Skill issue lol)

Bypassing the kernel eliminates syscall overhead, but it also throws away decades of kernel TCP tuning:

- **NAPI** batches interrupt coalescing and packet processing so the kernel already amortises per-packet cost at scale.
- **TSO/GSO/GRO** offloads let the NIC handle segmentation and reassembly in hardware — the hand-rolled stacks get none of this.
- The kernel TCP stack has a mature congestion controller, retransmission engine, and receive-window management. The custom stacks lack all of it.

For a workload that is TCP-handshake-heavy (short-lived connections, small payloads), the kernel's maturity outweighs the syscall savings.

### Why DPDK wins on pipelined requests

With pipelining, connection-setup overhead is amortised across many requests per connection. This removes `l4_impl`'s main advantage (mature kernel connection handling) while exposing DPDK's actual strength: zero-copy, zero-syscall packet I/O. At 50,753 req/s DPDK is ~1.3× faster than `l2_impl` in the pipelined case — consistent with DPDK eliminating the double kernel-boundary crossing that `l2_impl` (TAP device) still pays.

### Why `l2_impl` is slowest on standard requests

Every packet crosses the kernel boundary **twice** before reaching userspace: NIC → kernel network stack → TAP fd read → userspace. The custom TCP stack then processes frames that the kernel has already partially handled. This adds both copy overhead and scheduling latency that DPDK avoids entirely.

---

## Key Findings Per Branch

### `l4_impl`

Profiled with `perf` + flamegraph. Hottest paths:

- **`epoll_wait` (~26.8%)** — expected for event-driven I/O; the process spends most time blocked waiting for readiness.
- **`accept` (~14%)** — short-lived connection churn forces a full TCP handshake + fd allocation per request. Keep-alive would shift this cost to memory.
- **`unordered_map` (~18.3%)** — the connection table is in the hot path. File descriptors are already integers; a flat array indexed by fd would eliminate hashing and pointer chasing.
- **`pump_connection` (~9.9%)** — string copies (`substr`, `append`) on every request. `string_view`-based parsing would make header reads zero-copy.

### `dpdk`

Profiled with `perf` + flamegraph (~20 frames total — no kernel I/O paths):

- **`rtl_recv_pkts` (56.9%)** — the RTL8169 PMD draining the NIC RX ring dominated in earlier runs where the NIC was the bottleneck. With the added CPU load the processing cost is now more evenly distributed.
- **`unique_ptr` construction/destruction (~5.9%)** — the burst loop wraps each mbuf in a `unique_ptr`. DPDK's `rte_eth_rx_burst` returns up to 32 mbufs at once; processing them as a raw array and bulk-freeing after the burst would eliminate this overhead entirely.
- **`Connection: close` on every response** — every request pays a full 3-way SYN + FIN/ACK teardown. Keep-alive support would remove the round-trip cost and is why DPDK's advantage shows most clearly in the pipelined benchmark (50,753 req/s vs 15,399 without pipelining).

### `l2_impl`

Profiled with `perf` + flamegraph:

- **Socket allocation/destruction (~9.9%)** — every TCP connection heap-allocates a fresh `TCPSocket` (which holds a `std::deque`-backed queue); teardown shows up clearly. A pre-allocated socket pool would eliminate this.
- **`unordered_map::erase` (~6.3%)** — connection-close path erases from the active connection map on every close. Combined with string allocations in `parse_request` and `build_response_string`, the HTTP layer accounts for measurable overhead despite the NIC being the real bottleneck.

Each branch has its own `flamegraph.svg` with the full profile.

---

## Test Setup

**`l4_impl`** — standard Linux TCP socket on `0.0.0.0:8080`.

**`dpdk`** — NIC unbound from kernel driver and handed to DPDK via `vfio-pci`. Server hardcoded to `192.168.29.36:80`.

```sh
modprobe vfio-pci
dpdk-devbind --bind=vfio-pci <PCI_ADDR>
```

**`l2_impl`** — TAP device bridged with the physical NIC. Server hardcoded to `192.168.29.12:80`.

```sh
ip link add br0 type bridge
ip link set eth0 master br0
ip link set tap0 master br0
ip link set br0 up
```
