#include "dpdk.h"
#include "buff.h"

#include <cstdint>
#include <cstring>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <sys/types.h>

Dpdk::Dpdk(){
    setupDpdk();
    readMacAddr();
}

void Dpdk::setupDpdk(){
    mbuf_pool_ = rte_pktmbuf_pool_create(
		"MBUF_POOL", 
		NUM_MBUFS, 
		MBUF_CACHE_SIZE,
		0, 
		RTE_MBUF_DEFAULT_BUF_SIZE, 
		rte_socket_id()
	);

    if (mbuf_pool_ == NULL) rte_exit(EXIT_FAILURE, "Cannot create Mbuf mempool\n");
    if (portInit(port_id_, mbuf_pool_) !=0) 
		rte_exit(EXIT_FAILURE, "Cannot init port");
}

int Dpdk::portInit(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    struct rte_eth_dev_info dev_info;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    memset(&port_conf, 0, sizeof(struct rte_eth_conf));

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error getting device info (port %u): %s\n",
               port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0) return retval;

    // Adjust descriptor counts to what the HW supports
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0) return retval;

    retval = rte_eth_rx_queue_setup(port, 0, nb_rxd,
                                    rte_eth_dev_socket_id(port),
                                    NULL, mbuf_pool);
    if (retval != 0) return retval;

    retval = rte_eth_tx_queue_setup(port, 0, nb_txd,
                                    rte_eth_dev_socket_id(port), NULL);
    if (retval != 0) return retval;

    // Enable promiscuous BEFORE starting the port
    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0) return retval;

    retval = rte_eth_dev_start(port);
    if (retval != 0) return retval;

    // Shorter delay is fine now — promisc is already on
    rte_delay_ms(1000);

    struct rte_eth_link link;
    retval = rte_eth_link_get(port, &link);
    printf("Link %s - speed %u Mbps - Promiscuous: %s\n",
           link.link_status ? "UP" : "DOWN",
           link.link_speed,
           rte_eth_promiscuous_get(port) ? "ON" : "OFF");

    return 0;
}

void Dpdk::readMacAddr(){
    struct rte_ether_addr port_mac;
    rte_eth_macaddr_get(port_id_, &port_mac);

    mac_ = port_mac.addr_bytes;

}

pkt_buff_ptr Dpdk::recv_pkt(){
    if (buf_idx_ >= buf_count_) {
        buf_count_ = rte_eth_rx_burst(port_id_, 0, bufs_, BURST_SIZE);
        buf_idx_   = 0;
        if (buf_count_ == 0) return {nullptr, delete_buffer};
    }

    rte_mbuf* m  = bufs_[buf_idx_++];
    pkt_buff* pb = new pkt_buff{};
    uint8_t* base = static_cast<uint8_t*>(m->buf_addr);
    pb->head          = base;
    pb->data          = rte_pktmbuf_mtod(m, uint8_t*);
    pb->tail          = pb->data + m->data_len;
    pb->end           = base + m->buf_len;
    pb->native_handle = m;

    return pkt_buff_ptr(pb, [](pkt_buff* p){
        rte_pktmbuf_free(p->native_handle);
        delete p;
    });
}

ssize_t Dpdk::transmit(pkt_buff* buff){
    rte_mbuf* m = buff->native_handle;

    if (!m) {
        m = rte_pktmbuf_alloc(mbuf_pool_);
        if (!m) return -1;
        uint8_t* dst = rte_pktmbuf_mtod(m, uint8_t*);
        std::memcpy(dst, buff->data, buff->len());
        m->data_len = static_cast<uint16_t>(buff->len());
        m->pkt_len  = m->data_len;
    } else {
        m->data_off = static_cast<uint16_t>(buff->data - static_cast<uint8_t*>(m->buf_addr));  // buf_addr is void*, cast needed
        m->data_len = static_cast<uint16_t>(buff->len());
        m->pkt_len  = m->data_len;
    }

    uint16_t sent = rte_eth_tx_burst(port_id_, 0, &m, 1);
    if(sent>0) buff->native_handle = nullptr;
    return sent > 0 ? static_cast<ssize_t>(buff->len()) : -1;
}