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

> **Note:** A CPU-intensive operation was added per request so the benchmark exercises per-request processing cost rather than just busy-wait NIC polling (the NIC fills the RX ring slower than the CPU can drain it on this hardware).

### Standard

```
wrk -t8 -c200 -d30s http://192.168.29.36/hi --timeout 10s
Running 30s test @ http://192.168.29.36/hi
  8 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    10.16ms   63.84ms   1.09s    98.18%
    Req/Sec     1.75k     1.58k    6.05k    75.60%
  362442 requests in 28.90s, 23.50MB read
  Socket errors: connect 0, read 0, write 0, timeout 23
Requests/sec:  12540.03

wrk -t8 -c1000 -d30s http://192.168.29.36/hi --timeout 10s
Running 30s test @ http://192.168.29.36/hi
  8 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     9.25ms   64.28ms   2.06s    99.14%
    Req/Sec     2.24k     1.16k    6.97k    67.33%
  440900 requests in 28.63s, 28.59MB read
  Socket errors: connect 0, read 0, write 0, timeout 72
Requests/sec:  15398.94
```

### Pipelined

```
wrk -t4 -c100 -d30s --latency -s pipeline.lua http://192.168.29.36/hi
Running 30s test @ http://192.168.29.36/hi
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    47.89ms  188.60ms   2.00s    94.95%
    Req/Sec    12.45k     5.67k   29.44k    54.07%
  Latency Distribution
     50%   15.67ms
     75%   23.73ms
     90%   33.45ms
  1468342 requests in 28.93s, 95.22MB read
  Socket errors: connect 0, read 0, write 0, timeout 64
Requests/sec:  50753.32
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