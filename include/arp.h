#pragma once

#include "arp_cache.h"
#include "eth.h"
#include "handler.h"
#include "net_addr.h"

class ARPHandler: public ProtocolHandler{
    private:
        mac_addr_t mac_;
        ip4_addr_t ip_;
        EthernetHandler& eth_;
        ArpCache& arp_cache_;
    public:
        ARPHandler(const mac_addr_t& mac, const ip4_addr_t& ip, EthernetHandler& eth, ArpCache& cache);
        ~ARPHandler() = default;

        void handle_packet(pkt_buff *pkt) override;
        ssize_t transmit(pkt_buff *pkt);
};
