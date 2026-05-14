#pragma once

#include "arp_cache.h"
#include "buff.h"
#include "eth.h"
#include "handler.h"
#include "net_addr.h"

#include <cstdint>
#include <sys/types.h>
#include <unordered_map>

enum class IPProto: uint8_t {
    ICMP = 0x01,
    TCP  = 0x06,
    UDP  = 0x11,
};

class IPHandler: public ProtocolHandler{
    private:
        ip4_addr_t ip_;
        EthernetHandler& below_;
        ArpCache& arp_cache_;
        std::unordered_map<uint8_t, ProtocolHandler*> ip_handler_reg_;

    public:
        IPHandler(const ip4_addr_t& ip, EthernetHandler& below, ArpCache& cache);
        ~IPHandler() = default;

        void handle_packet(pkt_buff* pkt) override;
        ssize_t transmit(pkt_buff* pkt, IPProto proto);

        void register_protocol(uint8_t ip_proto, ProtocolHandler& proto);
        ProtocolHandler* get_handler(uint8_t ip_proto);
};
