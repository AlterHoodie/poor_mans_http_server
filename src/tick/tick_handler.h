#pragma once

#include "tick.h"

#include <array>
#include <atomic>
#include <cstdint>

struct TickStats {
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> gaps{0};
    uint64_t last_seq = 0;
    std::array<uint64_t, 1024> latest_price{};
};

struct UdpRecvStats {
    uint64_t datagrams = 0;
    uint64_t parsed_ok = 0;
    uint64_t bad_size = 0;
};

inline void handle_tick(const Tick& tick, TickStats& stats) {
    uint64_t cur = stats.ticks.load(std::memory_order_relaxed);
    if (cur > 0 && tick.seq != stats.last_seq + 1)
        stats.gaps.fetch_add(tick.seq - stats.last_seq - 1, std::memory_order_relaxed);
    stats.last_seq = tick.seq;

    if (tick.symbol_id < stats.latest_price.size())
        stats.latest_price[tick.symbol_id] = static_cast<uint64_t>(tick.price);

    stats.ticks.fetch_add(1, std::memory_order_relaxed);
}

inline Tick make_tick(uint64_t seq, uint32_t symbol_id, int32_t price, int32_t qty) {
    Tick tick{};
    tick.seq = seq;
    tick.ts_ns = seq * 1000;
    tick.symbol_id = symbol_id;
    tick.price = price;
    tick.qty = qty;
    return tick;
}
