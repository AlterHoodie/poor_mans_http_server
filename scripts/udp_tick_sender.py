#!/usr/bin/env python3
"""Send binary Tick datagrams to the UDP server (port 9000 by default).

Tick layout (must match src/tick/tick.h):
  uint64_t seq
  uint64_t ts_ns
  uint32_t symbol_id
  int32_t  price
  int32_t  qty

Modes
-----
Default (load):
    Sends --count ticks fire-and-forget.
    Use --threads N to send from N parallel sender threads simultaneously.
    Ctrl+C the server to see stats.

Latency (--latency):
    Requires server running with `./build/server udp echo`.
    --inflight 1  (default): classic sequential send→wait→measure RTT.
    --inflight N  (N > 1):   asyncio pipeline — N ticks in-flight at once,
                              replies matched by sequence number.
"""

import argparse
import asyncio
import socket
import statistics
import struct
import threading
import time
from dataclasses import dataclass, field

TICK_FMT  = "<QQIii"
TICK_SIZE = struct.calcsize(TICK_FMT)


def pack_tick(seq: int, ts_ns: int, symbol_id: int, price: int, qty: int) -> bytes:
    return struct.pack(TICK_FMT, seq, ts_ns, symbol_id, price, qty)


# ─── load mode ────────────────────────────────────────────────────────────────

@dataclass
class LoadResult:
    sent:    int = 0
    elapsed: float = 0.0


def _load_worker(
    host: str,
    port: int,
    start_seq: int,
    count: int,
    symbol: int,
    base_price: int,
    qty: int,
    rate: float,           # ticks/sec for this worker (0 = unlimited)
    result: LoadResult,
) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (host, port)
    interval = 1.0 / rate if rate > 0 else 0.0
    t0 = time.time_ns()

    for i in range(count):
        seq = start_seq + i
        payload = pack_tick(
            seq=seq,
            ts_ns=time.time_ns(),
            symbol_id=symbol,
            price=base_price + seq,
            qty=qty,
        )
        sock.sendto(payload, addr)
        if interval:
            time.sleep(interval)

    result.elapsed = (time.time_ns() - t0) / 1e9
    result.sent    = count
    sock.close()


def run_load(args: argparse.Namespace) -> None:
    n_threads  = max(1, args.threads)
    total      = args.count
    per_thread = total // n_threads
    remainder  = total % n_threads
    # Per-thread rate so the aggregate rate stays at args.rate.
    thread_rate = args.rate / n_threads if args.rate > 0 else 0.0

    results: list[LoadResult] = [LoadResult() for _ in range(n_threads)]
    threads: list[threading.Thread] = []

    global_start = time.time_ns()

    seq_offset = 1
    for idx in range(n_threads):
        count = per_thread + (1 if idx < remainder else 0)
        t = threading.Thread(
            target=_load_worker,
            args=(
                args.host, args.port,
                seq_offset, count,
                args.symbol, args.price, args.qty,
                thread_rate,
                results[idx],
            ),
            daemon=True,
        )
        seq_offset += count
        threads.append(t)

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    total_sent   = sum(r.sent for r in results)
    wall_elapsed = (time.time_ns() - global_start) / 1e9
    rate_actual  = total_sent / wall_elapsed if wall_elapsed > 0 else 0.0

    print(
        f"sent {total_sent} ticks ({TICK_SIZE} bytes each) "
        f"to {args.host}:{args.port} "
        f"in {wall_elapsed:.3f}s ({rate_actual:.0f} ticks/s) "
        f"across {n_threads} thread(s)"
    )
    print("Ctrl+C the server to see ticks/gaps stats.")


# ─── latency mode — sequential (inflight=1) ──────────────────────────────────

def run_latency_sequential(args: argparse.Namespace) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))
    sock.settimeout(args.timeout)
    addr = (args.host, args.port)

    rtts_ns: list[int] = []
    timeouts = 0
    start = time.time_ns()

    for seq in range(1, args.count + 1):
        send_ns = time.time_ns()
        payload = pack_tick(
            seq=seq,
            ts_ns=send_ns,
            symbol_id=args.symbol,
            price=args.price + seq,
            qty=args.qty,
        )
        sock.sendto(payload, addr)

        try:
            data, _ = sock.recvfrom(2048)
        except TimeoutError:
            timeouts += 1
            continue

        recv_ns = time.time_ns()

        if len(data) != TICK_SIZE:
            continue

        echoed = struct.unpack(TICK_FMT, data)
        if echoed[0] != seq:
            continue

        rtts_ns.append(recv_ns - send_ns)

        if args.rate > 0:
            time.sleep(1.0 / args.rate)

    sock.close()
    _print_latency_stats(args, start, rtts_ns, timeouts)


# ─── latency mode — pipelined asyncio (inflight > 1) ─────────────────────────

@dataclass
class _PipelineState:
    """Shared state between the asyncio sender and the datagram receiver."""
    send_times:  dict[int, int]  = field(default_factory=dict)  # seq → send_ns
    rtts_ns:     list[int]       = field(default_factory=list)
    timeouts:    int             = 0
    # Semaphore is created inside the loop; stored here for the protocol to access.
    sem:         asyncio.Semaphore | None = None


class _EchoProtocol(asyncio.DatagramProtocol):
    def __init__(self, state: _PipelineState) -> None:
        self._state = state
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.DatagramTransport) -> None:  # type: ignore[override]
        self.transport = transport

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        recv_ns = time.time_ns()
        if len(data) != TICK_SIZE:
            return
        echoed = struct.unpack(TICK_FMT, data)
        seq    = echoed[0]
        send_ns = self._state.send_times.pop(seq, None)
        if send_ns is None:
            return
        self._state.rtts_ns.append(recv_ns - send_ns)
        if self._state.sem is not None:
            self._state.sem.release()

    def error_received(self, exc: Exception) -> None:
        pass  # ICMP port-unreachable etc.

    def connection_lost(self, exc: Exception | None) -> None:
        pass


async def _run_pipeline(args: argparse.Namespace, state: _PipelineState) -> None:
    loop = asyncio.get_running_loop()
    state.sem = asyncio.Semaphore(args.inflight)

    # Create and bind the socket manually to avoid getaddrinfo("", 0) failures
    # that occur on some systems when asyncio resolves port 0 as a service name.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    sock.connect((args.host, args.port))
    sock.setblocking(False)

    transport, protocol = await loop.create_datagram_endpoint(
        lambda: _EchoProtocol(state),
        sock=sock,
    )

    interval = 1.0 / args.rate if args.rate > 0 else 0.0

    async def _timeout_watcher(seq: int, deadline: float) -> None:
        """Release the semaphore and count a timeout if no reply by deadline."""
        await asyncio.sleep(max(0.0, deadline - time.monotonic()))
        if seq in state.send_times:
            state.send_times.pop(seq, None)
            state.timeouts += 1
            if state.sem is not None:
                state.sem.release()

    tasks: list[asyncio.Task] = []

    for seq in range(1, args.count + 1):
        await state.sem.acquire()

        send_ns = time.time_ns()
        payload = pack_tick(
            seq=seq,
            ts_ns=send_ns,
            symbol_id=args.symbol,
            price=args.price + seq,
            qty=args.qty,
        )
        state.send_times[seq] = send_ns
        transport.sendto(payload)

        deadline = time.monotonic() + args.timeout
        tasks.append(asyncio.create_task(_timeout_watcher(seq, deadline)))

        if interval:
            await asyncio.sleep(interval)

    # Wait for all in-flight replies / timeouts to drain.
    await asyncio.gather(*tasks)
    transport.close()


def run_latency_pipeline(args: argparse.Namespace) -> None:
    state = _PipelineState()
    start = time.time_ns()
    asyncio.run(_run_pipeline(args, state))
    _print_latency_stats(args, start, state.rtts_ns, state.timeouts)


# ─── shared stats printer ─────────────────────────────────────────────────────

def _print_latency_stats(
    args: argparse.Namespace,
    start_ns: int,
    rtts_ns: list[int],
    timeouts: int,
) -> None:
    elapsed  = (time.time_ns() - start_ns) / 1e9
    received = len(rtts_ns)
    print(
        f"\nsent={args.count}  received={received}  timeouts={timeouts}  "
        f"elapsed={elapsed:.3f}s"
    )

    if rtts_ns:
        rtts_us     = [r / 1000 for r in rtts_ns]
        sorted_rtts = sorted(rtts_us)
        n           = len(sorted_rtts)
        p50         = sorted_rtts[int(n * 0.50)]
        p99         = sorted_rtts[min(int(n * 0.99), n - 1)]
        print(
            f"RTT (µs)  min={sorted_rtts[0]:.1f}  "
            f"avg={statistics.mean(rtts_us):.1f}  "
            f"p50={p50:.1f}  "
            f"p99={p99:.1f}  "
            f"max={sorted_rtts[-1]:.1f}"
        )
    else:
        print("No replies received — is the server running with `udp echo`?")


# ─── CLI ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="UDP tick sender / latency tester")
    parser.add_argument("--host",     default="127.0.0.1", help="receiver host")
    parser.add_argument("--port",     type=int,   default=9000,  help="receiver port")
    parser.add_argument("--count",    type=int,   default=10,    help="number of ticks to send")
    parser.add_argument("--rate",     type=float, default=0.0,   help="ticks/sec (0 = no delay)")
    parser.add_argument("--symbol",   type=int,   default=1,     help="symbol_id")
    parser.add_argument("--price",    type=int,   default=10050, help="price (fixed-point)")
    parser.add_argument("--qty",      type=int,   default=100,   help="quantity")
    parser.add_argument(
        "--latency", action="store_true",
        help="latency mode: send ticks and measure RTT (requires server udp echo)",
    )
    parser.add_argument(
        "--threads", type=int, default=1, metavar="N",
        help="(load mode) number of parallel sender threads (default: 1)",
    )
    parser.add_argument(
        "--inflight", type=int, default=1, metavar="N",
        help="(latency mode) max ticks in-flight at once; >1 uses asyncio pipeline (default: 1)",
    )
    parser.add_argument(
        "--timeout", type=float, default=1.0,
        help="per-tick recv timeout in seconds for latency mode (default: 1.0)",
    )
    args = parser.parse_args()

    if args.latency:
        if args.inflight > 1:
            print(
                f"Latency mode — asyncio pipeline, "
                f"inflight={args.inflight}, count={args.count}, "
                f"host={args.host}:{args.port}"
            )
            run_latency_pipeline(args)
        else:
            print(
                f"Latency mode — sequential, "
                f"count={args.count}, host={args.host}:{args.port}"
            )
            run_latency_sequential(args)
    else:
        run_load(args)


if __name__ == "__main__":
    main()
