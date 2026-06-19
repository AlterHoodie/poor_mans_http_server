#pragma once

#include "tick.h"

#include <array>
#include <atomic>
#include <cstdint>

struct TickStats {
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> gaps{0};
    uint64_t last_seq{0};
    std::array<uint64_t, 1024> latest_price{};
};

// Synthetic per-packet work to slow drain rate without yielding the CPU.
// Tune work_iters so processing time roughly matches inter-packet gap.
inline uint64_t burn_cpu(uint64_t seed, int work_iters) {
    volatile uint64_t x = seed;
    for (int i = 0; i < work_iters; ++i)
        x = x * 6364136223846793005ULL + 1;
    return x;
}

inline void handle_tick(const Tick& tick, TickStats& stats, int work_iters = 0) {
    uint64_t cur = stats.ticks.load(std::memory_order_relaxed);
    if (cur > 0 && tick.seq != stats.last_seq + 1)
        stats.gaps.fetch_add(tick.seq - stats.last_seq - 1, std::memory_order_relaxed);
    stats.last_seq = tick.seq;

    if (tick.symbol_id < stats.latest_price.size())
        stats.latest_price[tick.symbol_id] = static_cast<uint64_t>(tick.price);

    stats.ticks.fetch_add(1, std::memory_order_relaxed);

    if (work_iters > 0)
        (void)burn_cpu(tick.seq, work_iters);
}
