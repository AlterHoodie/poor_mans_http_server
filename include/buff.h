#pragma once

#include <cstdint>

#define PKT_ALLOCATION_SIZE 2048


struct pkt_buff{
    uint8_t *head;

    uint8_t *data;
    uint8_t *tail;

    uint8_t *end;

    std::size_t len() const {
        return tail - data;
    }

    std::size_t headroom() const {
        return data - head;
    }

    std::size_t tailroom() const {
        return end - tail;
    }
};

bool pull(pkt_buff* b, std::size_t len);
bool push(pkt_buff* b, std::size_t len);

pkt_buff* create_buffer();