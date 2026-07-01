# poor_mans_http_server

A userspace HTTP server backed by a barely working TCP/IP stack  implemented using DPDK. The stack starts at Layer 2 : it reads raw Ethernet frames and implements everything up the chain itself: ARP, IPv4, ICMP, TCP, and a minimal HTTP layer. Everything below Ethernet (physical link, drivers, kernel networking) is abstracted away.

The kernel is bypassed entirely at the I/O layer via DPDK. A tight `while(true)` loop calls `rte_eth_rx_burst` directly against the NIC : no syscalls, no scheduler involvement. TCP connections surface as callbacks (`on_accept`/`on_data`/`on_close`) that fire synchronously on the same call stack as the NIC drain loop (Ofcourse a bottleneck - a better approach would delegate the application processing to a different core altogether).

## Stack

```
NIC (DPDK PMD)
         |
     Ethernet
     /       \
   ARP        IP
             /  \
          ICMP   TCP
                  |
                HTTP
```

## Ideal Pipeline

Right now RX, protocol processing, and TX all happen on one core inline. The better design pins each stage to a dedicated core and connects them with lockless ring buffers — specifically DPDK's `rte_ring`, a circular buffer in hugepage memory that two cores can read/write without any kernel involvement. Pipes are out because they're kernel fds; using them would bring back the syscall overhead DPDK was meant to eliminate.

```
                               NIC
                   ┌────────────┴────────────┐
                   ▼                         ▲
                RX core                   TX core
                   │                         ▲
rte_ring (inbound) │                         │ rte_ring (outbound)
                   ▼                         │
                   └──────── App core ───────┘
```

RX and TX each sit closest to the NIC, busy-polling their respective queues via `rte_eth_rx_burst` / `rte_eth_tx_burst`. 
The app core sits below, decoupled from both by `rte_ring` — 

- RX enqueues parsed packets into an inbound ring, 
- app dequeues them, processes (HTTP parse → route → response)
- enqueues response mbufs into an outbound ring that TX drains.

Each core runs a tight busy-poll loop on its own ring. The RX core enqueues parsed packets, the app core dequeues them, processes them, and enqueues responses, and the TX core drains the response ring to the NIC. No locks, no context switches, no shared mutable state between stages.

## Test Setup

Tested on a home server running Linux. The NIC was unbound from the kernel driver and handed to DPDK via `vfio-pci`:

```sh
# included in scripts/
modprobe vfio-pci
dpdk-devbind --bind=vfio-pci <PCI_ADDR>
```

Server IP is hardcoded to `192.168.29.36:80`.

**AWS** — server on `c6i`, load generator on `c7n`, same VPC and availability zone.

## Benchmark

**Local (home server):** on a consumer RTL8169 NIC with a Wi-Fi laptop as the load generator, throughput lands in the same ~8k–15k req/s ballpark as plain sockets and TAP — the cheap NIC and underpowered client are the ceiling, not the implementation.

**AWS HTTP (`wrk -t8 -c1000 -d30s`, keep-alive):**

| Metric  | Value                                              |
| ------- | -------------------------------------------------- |
| Req/sec | ~120,000                                           |
| p50     | 6.6–8.4 ms                                         |
| p99     | ~6.8–8.7 ms (flat, within ~15% of p50)             |

Tail latency stays nearly flat under load — no epoll wakeup, no kernel scheduler on the data path, just a continuous userspace poll loop doing fixed per-packet work.

**UDP flood:**

| Tier  | Packets/sec                                                                 |
| ----- | --------------------------------------------------------------------------- |
| Local | ~72,000 — client cannot push fast enough; CPU sits in the driver poll loop  |
| AWS   | ~1,000,000 — ENA/instance PPS ceiling (client sent ~1.4M; loss unchanged when throttled) |

## Flamegraph

See [flamegraph.svg](flamegraph.svg). The profile is flat locally — roughly 20 distinct frames total, because there are no kernel I/O paths to traverse.

### Local c1000 (HTTP)

| Frame | % | What it means |
|---|---|---|
| `rtl_recv_pkts` | ~57% | RTL8169 DPDK PMD draining the NIC RX ring — CPU waiting for packets |
| `Dpdk::recv_pkt` | ~25% | Poll loop on top of `rte_eth_rx_burst` |
| Application (TCP/HTTP) | ~2% | Hand-rolled stack barely registers — NIC is the ceiling |
| `unique_ptr` construction/destruction | ~5.9% | Per-packet smart pointer overhead in the burst loop |

### AWS c1000 (HTTP)

The bottleneck inverts once the NIC can keep up:

| Frame | % | What it means |
|---|---|---|
| ENA driver | ~8–9% | NIC is no longer the limiting factor |
| `TCPHandler::handle_established` | ~50% | Hand-rolled TCP stack is now the hot path |
| Deque / segment transmission | ~47% | Send buffer management inside the custom TCP stack |

Locally, DPDK waits efficiently for a NIC that cannot keep up. On AWS, the NIC feeds packets faster than this program's TCP stack can acknowledge and transmit responses — the bottleneck moves to the one piece that is entirely homegrown.

## Findings & Improvements

### Burst RX loop — replace per-packet `unique_ptr` with a raw mbuf array

The current loop processes one packet at a time:

```cpp
while (true) {
    auto pkt = dpdk.recv_pkt();   // one unique_ptr per packet
    if (pkt) eth_handler.handle_packet(pkt.get());
}
```

DPDK is designed around burst processing — `rte_eth_rx_burst` returns up to 32 mbufs in a single call. Wrapping each in a `unique_ptr` and processing them sequentially negates the burst benefit. The local flamegraph shows ~5.9% cost from this. A burst-aware loop holding a raw `pkt_buff*[32]` array and bulk-freeing after processing keeps adjacent packets warm in cache and eliminates all smart pointer overhead.

### Skill issues

- **Double `parse_request()` call**: in `pump_connection`, the request is parsed once to extract `Content-Length`, then parsed again in full during `PROCESSING`. The first `Request` should be stored on `HTTPConnection` and reused.
- **Substring allocation for header parse**: `conn.read_buf.substr(0, conn.body_start)` heap-allocates a copy of the header block just to hand to the parser. Changing `parse_request` to accept `std::string_view` makes it a zero-cost slice.
- **`build_response_string` lacks `reserve()`**: each `+=` can trigger a reallocation. The final size is computable upfront; one `reserve()` before the first append eliminates all of them.
- **Byte-by-byte copy from `deque` in `transmit`**: the send loop iterates `deque<uint8_t>` one byte at a time with a branch per byte. `std::copy` over deque iterators gives the compiler enough information to vectorize.
- **ACK processing calls `pop_front()` per byte**: the ACK handler loops `bytes_acked` times calling `pop_front()`. A single range `erase(begin, begin + bytes_acked)` is equivalent and does it in one shot.
- **Hardcoded ISN of `1000`**: RFC 6528 requires a cryptographically unpredictable Initial Sequence Number. The fixed value is a security bug — use `rte_rand()` or a properly seeded PRNG.
