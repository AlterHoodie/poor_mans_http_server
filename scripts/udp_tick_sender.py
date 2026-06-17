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
    Sends --count ticks fire-and-forget. Ctrl+C the server to see stats.

Latency (--latency):
    Requires server running with `./build/server udp echo`.
    Sends one tick at a time and waits for the echoed reply, measuring RTT.
"""

import argparse
import socket
import statistics
import struct
import time

TICK_FMT = "<QQIii"
TICK_SIZE = struct.calcsize(TICK_FMT)


def pack_tick(seq: int, ts_ns: int, symbol_id: int, price: int, qty: int) -> bytes:
    return struct.pack(TICK_FMT, seq, ts_ns, symbol_id, price, qty)


def run_load(args: argparse.Namespace, sock: socket.socket, addr: tuple) -> None:
    interval = 1.0 / args.rate if args.rate > 0 else 0.0
    start = time.time_ns()

    for seq in range(1, args.count + 1):
        payload = pack_tick(
            seq=seq,
            ts_ns=time.time_ns(),
            symbol_id=args.symbol,
            price=args.price + seq,
            qty=args.qty,
        )
        sock.sendto(payload, addr)

        if interval:
            time.sleep(interval)

    elapsed = (time.time_ns() - start) / 1e9
    rate = args.count / elapsed if elapsed > 0 else 0.0
    print(
        f"sent {args.count} ticks ({TICK_SIZE} bytes each) "
        f"to {args.host}:{args.port} in {elapsed:.3f}s ({rate:.0f} ticks/s)"
    )
    print("Ctrl+C the server to see ticks/gaps stats.")


def run_latency(args: argparse.Namespace, sock: socket.socket, addr: tuple) -> None:
    sock.bind(("", 0))
    sock.settimeout(args.timeout)

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

    elapsed = (time.time_ns() - start) / 1e9
    received = len(rtts_ns)
    print(
        f"\nsent={args.count}  received={received}  timeouts={timeouts}  "
        f"elapsed={elapsed:.3f}s"
    )

    if rtts_ns:
        rtts_us = [r / 1000 for r in rtts_ns]
        sorted_rtts = sorted(rtts_us)
        n = len(sorted_rtts)
        p50 = sorted_rtts[int(n * 0.50)]
        p99 = sorted_rtts[min(int(n * 0.99), n - 1)]
        print(
            f"RTT (µs)  min={sorted_rtts[0]:.1f}  "
            f"avg={statistics.mean(rtts_us):.1f}  "
            f"p50={p50:.1f}  "
            f"p99={p99:.1f}  "
            f"max={sorted_rtts[-1]:.1f}"
        )
    else:
        print("No replies received — is the server running with `udp echo`?")


def main() -> None:
    parser = argparse.ArgumentParser(description="UDP tick sender / latency tester")
    parser.add_argument("--host", default="127.0.0.1", help="receiver host")
    parser.add_argument("--port", type=int, default=9000, help="receiver port")
    parser.add_argument("--count", type=int, default=10, help="number of ticks to send")
    parser.add_argument("--rate", type=float, default=0.0, help="ticks/sec (0 = no delay)")
    parser.add_argument("--symbol", type=int, default=1, help="symbol_id")
    parser.add_argument("--price", type=int, default=10050, help="price (fixed-point)")
    parser.add_argument("--qty", type=int, default=100, help="quantity")
    parser.add_argument(
        "--latency", action="store_true",
        help="latency mode: send one tick at a time and measure RTT (requires server udp echo)"
    )
    parser.add_argument(
        "--timeout", type=float, default=1.0,
        help="per-tick recv timeout in seconds for latency mode (default: 1.0)"
    )
    args = parser.parse_args()

    addr = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if args.latency:
        run_latency(args, sock, addr)
    else:
        run_load(args, sock, addr)

    sock.close()


if __name__ == "__main__":
    main()
