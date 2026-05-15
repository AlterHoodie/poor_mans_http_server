#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#define PKT_ALLOCATION_SIZE 2048


struct pkt_buff{
    uint8_t *head;

    uint8_t *data;
    uint8_t *tail;

    uint8_t *end;

    struct rte_mbuf *native_handle = nullptr;

    // filled by the IP layer before it strips its header
    uint8_t ip_src[4]{};
    uint8_t ip_dst[4]{};

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

using pkt_buff_ptr = std::unique_ptr<pkt_buff, void(*)(pkt_buff*)>;
pkt_buff_ptr create_buffer();
void delete_buffer(pkt_buff* buff);
void buff_pool_init(struct rte_mempool* pool);