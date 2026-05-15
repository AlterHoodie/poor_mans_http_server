#pragma once

#include "buff.h"
#include "net_addr.h"

#include <cstdint>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250

#define BURST_SIZE 32

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

class Dpdk {
public:
    Dpdk();

    const uint8_t* mac() const { return mac_.data(); }

    pkt_buff_ptr recv_pkt();
    ssize_t      transmit(pkt_buff* buff);
    rte_mempool* pool() {return mbuf_pool_;};

private:
    uint16_t port_id_ = 0;
    mac_addr_t mac_;
    struct rte_mempool* mbuf_pool_{nullptr};

    struct rte_mbuf* bufs_[BURST_SIZE];
    uint16_t buf_count_{0};
    uint16_t buf_idx_{0};

    void setupDpdk();
    int  portInit(uint16_t port, struct rte_mempool* mbuf_pool);
    void readMacAddr();
};