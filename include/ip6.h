#pragma once

#include "buff.h"
#include "eth.h"
#include "handler.h"
#include "ip.h"
#include "ndp_cache.h"
#include "net_addr.h"

#include <cstdint>
#include <unordered_map>

class IP6Handler : public ProtocolHandler {
private:
    ip6_addr_t ip_;
    EthernetHandler& below_;
    NdpCache& ndp_cache_;
    std::unordered_map<uint8_t, ProtocolHandler*> ip_handler_reg_;

public:
    IP6Handler(const ip6_addr_t& ip, EthernetHandler& below, NdpCache& cache);
    ~IP6Handler() = default;

    void handle_packet(pkt_buff* pkt) override;
    ssize_t transmit(pkt_buff* pkt, IPProto proto);

    void register_protocol(uint8_t ip_proto, ProtocolHandler& proto);
    ProtocolHandler* get_handler(uint8_t ip_proto);
};
