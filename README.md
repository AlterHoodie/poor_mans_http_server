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

## Benchmark

```
wrk -t4 -c100 -d30s http://192.168.29.36:80/hi
```

These were the throughput results:

```
  Latency   239.96ms   51.75ms 757.81ms   97.24%
  Req/Sec    59.35     38.12   222.00     65.15%
  6935 requests in 30.08s, 426.67KB read
Requests/sec: 230.58
```

## Flamegraph

See [flamegraph.svg](flamegraph.svg). The profile is flat — roughly 20 distinct frames total, because there are no kernel I/O paths to traverse.

| Frame | % | What it means |
|---|---|---|
| `std::unique_ptr<pkt_buff, ...>` | 93.25% | Entire per-packet processing scope — everything collapses under this one lifetime |
| `rtl_recv_pkts` | 56.88% | RTL8169 DPDK PMD draining the NIC RX ring; more than half of CPU is in the driver |
| `Dpdk::recv_pkt` | 36.17% | Poll loop on top of `rte_eth_rx_burst`, managing burst buffer index and mbuf handoff |
| `std::tuple<pkt_buff*, void...>` destructor | 2.93% | `rte_pktmbuf_free` on consumed mbufs — the only measurable application overhead |
| `std::_Head_base<0ul, pkt_buff*...>` | 2.16% | `unique_ptr` construction cost |
| `std::_Tuple_impl<1ul, void...>` | 0.77% | Custom deleter binding inside `unique_ptr` |

The bottleneck is the NIC driver. TCP, HTTP parsing, and routing do not register at a measurable percentage — the NIC poll rate is the ceiling. The ~5.86% combined `unique_ptr` construction/destruction overhead is the only purely application-side cost visible in the profile.

## Findings & Improvements

### Burst RX loop — replace per-packet `unique_ptr` with a raw mbuf array

The current loop processes one packet at a time:

```cpp
while (true) {
    auto pkt = dpdk.recv_pkt();   // one unique_ptr per packet
    if (pkt) eth_handler.handle_packet(pkt.get());
}
```

DPDK is designed around burst processing — `rte_eth_rx_burst` returns up to 32 mbufs in a single call. Wrapping each in a `unique_ptr` and processing them sequentially negates the burst benefit. The flamegraph already shows the ~5.86% cost of this. A burst-aware loop holding a raw `pkt_buff*[32]` array and bulk-freeing after processing keeps adjacent packets warm in cache and eliminates all smart pointer overhead.

### Keep-alive — every request pays a full handshake + teardown

`build_response_string` hardcodes `Connection: close` and `main` calls `tcp_close` immediately after each response. Every HTTP request therefore pays a 3-way SYN handshake plus a FIN/ACK teardown. Supporting `Connection: keep-alive` eliminates that round-trip cost for any client making more than one request — which browsers and benchmarking tools (`wrk`, `ab`) always do. This is the most likely explanation for the 239ms p50 latency in the benchmark.

### Skill issues

- **Double `parse_request()` call**: in `pump_connection`, the request is parsed once to extract `Content-Length`, then parsed again in full during `PROCESSING`. The first `Request` should be stored on `HTTPConnection` and reused.
- **Substring allocation for header parse**: `conn.read_buf.substr(0, conn.body_start)` heap-allocates a copy of the header block just to hand to the parser. Changing `parse_request` to accept `std::string_view` makes it a zero-cost slice.
- **`build_response_string` lacks `reserve()`**: each `+=` can trigger a reallocation. The final size is computable upfront; one `reserve()` before the first append eliminates all of them.
- **Byte-by-byte copy from `deque` in `transmit`**: the send loop iterates `deque<uint8_t>` one byte at a time with a branch per byte. `std::copy` over deque iterators gives the compiler enough information to vectorize.
- **ACK processing calls `pop_front()` per byte**: the ACK handler loops `bytes_acked` times calling `pop_front()`. A single range `erase(begin, begin + bytes_acked)` is equivalent and does it in one shot.
- **Hardcoded ISN of `1000`**: RFC 6528 requires a cryptographically unpredictable Initial Sequence Number. The fixed value is a security bug — use `rte_rand()` or a properly seeded PRNG.