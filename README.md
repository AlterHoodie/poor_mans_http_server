# poor_mans_http_server

A learning exercise exploring how an HTTP server behaves at different layers of the networking stack. Three branches implement the same `GET /hi → 200 OK` endpoint, each starting I/O at a different level — from the kernel's TCP socket API down to raw Ethernet frames read directly off the NIC.

---

## Branch Index

| Branch                          | I/O mechanism                         | Stack starts at              | AWS req/sec (c1000) |
| ------------------------------- | ------------------------------------- | ---------------------------- | ------------------- |
| `[l4_impl](../../tree/l4_impl)` | Linux `epoll` + kernel TCP sockets    | Layer 4 — kernel owns TCP/IP | ~54,000             |
| `[dpdk](../../tree/dpdk)`       | DPDK `rte_eth_rx_burst` — no syscalls | Layer 2 — custom ARP/IP/TCP  | ~120,000            |
| `[l2_impl](../../tree/l2_impl)` | Linux TAP device (`tap0`)             | Layer 2 — custom ARP/IP/TCP  | —                   |

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

`l2_impl` crosses the kernel boundary twice per packet (NIC → kernel → TAP fd → userspace). DPDK bypasses the kernel entirely on the data path.

---

## Benchmark Comparison

### Local (home server)

Tested on a home server with a consumer RTL8169 NIC and a Wi-Fi laptop as the load generator. Locally, all three implementations land in roughly the same throughput ballpark (~8k–15k req/s depending on the run). The bottleneck is the test rig — the NIC and client cannot produce enough packets to stress any implementation — not the code itself.

### AWS VPC (c6i server, c7n load generator, same AZ)

To separate implementations by their own merits, HTTP and UDP benchmarks were re-run in an AWS VPC with a compute-optimized server and a network-optimized load generator. Only `l4_impl` and `dpdk` were compared on AWS; the TAP-based `l2_impl` path does not work cleanly on Nitro/ENA virtualized networking (no straightforward host bridge for TAP traffic).

**HTTP (`wrk -t8 -c1000 -d30s`, keep-alive):**

| Implementation | Req/sec   | p50        | p99                          |
| -------------- | --------- | ---------- | ---------------------------- |
| `l4_impl`      | ~54,000   | 12–16 ms   | 35 ms – 1 s+ spikes          |
| `dpdk`         | ~120,000  | 6.6–8.4 ms | ~6.8–8.7 ms (flat, ~15% of p50) |

On AWS, DPDK delivers more than double the throughput of plain sockets, with tail latency that stays nearly flat under load. The socket server depends on the kernel scheduler, kernel TCP bookkeeping, and `write()` → `tcp_sendmsg` — each a place where multi-millisecond delay can appear under thousands of concurrent connections. DPDK has no epoll wakeup and no scheduler on the data path; a saturated userspace poll loop has nowhere for tail gaps to hide.

**UDP flood (packets/sec):**

| Tier   | `l4_impl`                                      | `dpdk`                                                       |
| ------ | ---------------------------------------------- | ------------------------------------------------------------ |
| Local  | `recvfrom` ~50%, `epoll_wait` ~48% — idle waiting | ~72,000 pps                                                  |
| AWS    | ~700,000 pps (kernel loss)                     | ~1,000,000 pps (ENA/instance PPS ceiling; client sent ~1.4M) |

The DPDK UDP plateau at ~1M pps is an AWS infrastructure limit (ENA per-instance packet rate cap), not an application bottleneck — loss stayed identical whether the client sent 1.4M or was throttled lower.

---

## Key Findings Per Branch

Each branch has its own `flamegraph.svg`. Percentages below are from labeled `perf` profiles; local and AWS profiles tell different stories because the bottleneck moves once the NIC can actually keep up.

### `l4_impl`

- **Local c1000:** `pump_connection` 60–70%, `parse_request` ~25%, `write` 13–16%; `epoll_wait` barely registers — the thread is busy servicing ready connections.
- **Local UDP:** `recvfrom` ~50%, `epoll_wait` ~48% — no HTTP work; the server waits for packets the client cannot push fast enough.
- **AWS c1000:** `pump_connection` ~72% (vs ~63% local) — faster network delivers more work to the application layer; `epoll_wait` stays negligible.

### `dpdk`

- **Local c1000:** `rtl_recv_pkts` ~57%, poll loop ~25%, application ~2% — the cheap consumer NIC is the ceiling; TCP and HTTP barely register.
- **AWS c1000:** ENA driver ~8–9%, `TCPHandler::handle_established` ~50%, deque/segment tx ~47% — bottleneck inverts from hardware to the hand-rolled TCP stack.
- **Burst RX:** wrapping each mbuf in a `unique_ptr` adds ~5.9% overhead; a raw `pkt_buff*[32]` burst loop would eliminate it.

### `l2_impl`

- **Local c1000:** `epoll_wait` ~45%, `pump_connection` <2%, `parse_request` <1% — most time waiting on the TAP fd or doing TCP bookkeeping, not HTTP parsing.
- **Not tested on AWS** — TAP + Nitro/ENA bridging is impractical on EC2.

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

**AWS** — server on `c6i`, load generator on `c7n`, same VPC and availability zone.
