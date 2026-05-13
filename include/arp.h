#pragma once

#include "eth.h"
#include "handler.h"
#include "net_addr.h"

class ARPHandler: public ProtocolHandler{
    private:
        mac_addr_t mac_;
        ip4_addr_t ip_;
        EthernetHandler& below_;
    public:
        ARPHandler(const mac_addr_t& mac, const ip4_addr_t& ip, EthernetHandler& below);
        ~ARPHandler() = default;

        void handle_packet(pkt_buff *pkt) override;
        ssize_t transmit(pkt_buff *pkt);
};