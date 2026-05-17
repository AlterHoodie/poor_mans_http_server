# poor_mans_http_server

A learning exercise exploring how an HTTP server behaves at different layers of the networking stack. Three branches implement the same `GET /hi → 200 OK` endpoint, each starting I/O at a different level — from the kernel's TCP socket API down to raw Ethernet frames read directly off the NIC.

---

## Branch Index

| Branch | I/O mechanism | Stack starts at | Req/sec |
|---|---|---|---|
| [`l4_impl`](../../tree/l4_impl) | Linux `epoll` + kernel TCP sockets | Layer 4 — kernel owns TCP/IP | **350.89** |
| [`dpdk`](../../tree/dpdk) | DPDK `rte_eth_rx_burst` — no syscalls | Layer 2 — custom ARP/IP/TCP | 230.58 |
| [`l2_impl`](../../tree/l2_impl) | Linux TAP device (`tap0`) | Layer 2 — custom ARP/IP/TCP | 59.52 |

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

All runs: `wrk -t4 -c100 -d30s`, home server, same machine.

| Branch | Req/sec | Avg latency | Max latency | Timeouts |
|---|---|---|---|---|
| `l4_impl` | **350.89** | 87.26 ms | 1.99 s | 54 |
| `dpdk` | 230.58 | 239.96 ms | 757.81 ms | 0 |
| `l2_impl` | 59.52 | 619.12 ms | 1.99 s | 480 |

### Why `l4_impl` wins despite DPDK

Bypassing the kernel eliminates syscall overhead, but it also throws away decades of kernel TCP tuning:

- **NAPI** batches interrupt coalescing and packet processing so the kernel already amortises per-packet cost at scale.
- **TSO/GSO/GRO** offloads let the NIC handle segmentation and reassembly in hardware — the hand-rolled stacks get none of this.
- The kernel TCP stack has a mature congestion controller, retransmission engine, and receive-window management. The custom stacks lack all of it.

For a workload that is TCP-handshake-heavy (short-lived connections, small payloads), the kernel's maturity outweighs the syscall savings.

### Why `l2_impl` is slowest

Every packet crosses the kernel boundary **twice** before reaching userspace: NIC → kernel network stack → TAP fd read → userspace. The custom TCP stack then processes frames that the kernel has already partially handled. This adds both copy overhead and scheduling latency that DPDK avoids entirely — which is why `dpdk` (230 req/s) is still 4× faster than `l2_impl` (59 req/s) even with a weaker TCP implementation.

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

- **`rtl_recv_pkts` (56.9%)** — the RTL8169 PMD draining the NIC RX ring dominates. The NIC poll rate is the ceiling; TCP and HTTP parsing don't register.
- **`unique_ptr` construction/destruction (~5.9%)** — the burst loop wraps each mbuf in a `unique_ptr`. DPDK's `rte_eth_rx_burst` returns up to 32 mbufs at once; processing them as a raw array and bulk-freeing after the burst would eliminate this overhead entirely.
- **`Connection: close` on every response** — every request pays a full 3-way SYN + FIN/ACK teardown. This is the most likely cause of the 240 ms p50 latency. Keep-alive support would remove the round-trip cost.

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
