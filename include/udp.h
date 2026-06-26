#pragma once

#include "buff.h"
#include "handler.h"
#include "ip.h"
#include "ip6.h"
#include "tick.h"
#include "tick_handler.h"

#include <cstdint>
#include <unordered_set>

class UDPHandler : public ProtocolHandler {
private:
    IPHandler&                   ip4_;
    IP6Handler&                  ip6_;
    std::unordered_set<uint16_t> bind_table_;
    bool                         echo_{false};
    int                          work_iters_{0};

public:
    TickStats stats{};
    uint64_t  bad_size{0};
    uint64_t  hdr_drop{0};

    UDPHandler(IPHandler& ip4, IP6Handler& ip6);
    ~UDPHandler() = default;

    void    handle_packet(pkt_buff* pkt) override;
    ssize_t transmit(pkt_buff* pkt);

    void udp_bind(uint16_t port);
    void set_echo(bool echo);
    void set_work_iters(int work_iters);
};
