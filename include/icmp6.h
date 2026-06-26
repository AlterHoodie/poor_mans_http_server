#pragma once

#include "handler.h"
#include "ip6.h"
#include "ndp_cache.h"
#include "net_addr.h"

enum class ICMP6Type : uint8_t {
    EchoRequest          = 128,
    EchoReply            = 129,
    NeighborSolicitation = 135,
    NeighborAdvertisement = 136,
};

class ICMP6Handler : public ProtocolHandler {
private:
    IP6Handler& ip6_;
    NdpCache&   ndp_cache_;
    ip6_addr_t  ip_;
    mac_addr_t  mac_;

    void learn_options(const uint8_t* icmp, size_t len, const ip6_addr_t& ip);
    ssize_t send_neighbor_advertisement(pkt_buff* pkt, const ip6_addr_t& target);

public:
    ICMP6Handler(const mac_addr_t& mac, const ip6_addr_t& ip,
                 IP6Handler& ip6, NdpCache& cache);
    ~ICMP6Handler() = default;

    void handle_packet(pkt_buff* pkt) override;
    ssize_t transmit_echo_reply(pkt_buff* pkt);
};
