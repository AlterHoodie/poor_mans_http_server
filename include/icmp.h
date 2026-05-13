#pragma once

#include "handler.h"
#include "ip.h"
#include "net_addr.h"

enum class ICMPType : uint8_t {
    EchoReply   = 0,
    EchoRequest = 8,
};

class ICMPHandler : public ProtocolHandler {
private:
    IPHandler& ip_;

public:
    explicit ICMPHandler(IPHandler& ip);
    ~ICMPHandler() = default;

    void handle_packet(pkt_buff* pkt) override;
    ssize_t transmit(pkt_buff* pkt);
};
