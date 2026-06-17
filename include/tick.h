#pragma once

#include <cstdint>

struct [[gnu::packed]] Tick {
    uint64_t seq;
    uint64_t ts_ns;
    uint32_t symbol_id;
    int32_t  price;
    int32_t  qty;
};

static_assert(sizeof(Tick) == 28);
