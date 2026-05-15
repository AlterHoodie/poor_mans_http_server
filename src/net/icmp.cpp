#include "icmp.h"
#include "buff.h"
#include "ip.h"
#include "utils.h"

#include <cstdint>
#include <cstring>
#include <sys/types.h>

ICMPHandler::ICMPHandler(IPHandler& ip) : ip_(ip) {}

void ICMPHandler::handle_packet(pkt_buff* pkt) {
    /*
        ICMP message layout
        -------------------
        Type       : 1 byte   [0]
        Code       : 1 byte   [1]
        Checksum   : 2 bytes  [2-3]
        Identifier : 2 bytes  [4-5]
        Sequence   : 2 bytes  [6-7]
        Data       : variable [8+]
    */

    constexpr std::size_t kIcmpMinLen = 8;
    if (pkt->len() < kIcmpMinLen) {
        return;
    }

    uint8_t* icmp = pkt->data;

    if (icmp[0] != static_cast<uint8_t>(ICMPType::EchoRequest)) {
        return;
    }
    if (icmp[1] != 0) {
        return;
    }

    // verify checksum
    const uint16_t orig_csum = (static_cast<uint16_t>(icmp[2]) << 8) | icmp[3];
    icmp[2] = 0;
    icmp[3] = 0;
    if (internet_checksum(icmp, pkt->len()) != orig_csum) {
        return;
    }

    transmit(pkt);
}

ssize_t ICMPHandler::transmit(pkt_buff* pkt) {
    uint8_t* icmp = pkt->data;

    // flip type: EchoRequest(8) -> EchoReply(0), code unchanged
    icmp[0] = static_cast<uint8_t>(ICMPType::EchoReply);
    icmp[1] = 0;

    // checksum field already zeroed in handle_packet; recompute over full message
    icmp[2] = 0;
    icmp[3] = 0;
    const uint16_t csum = htons(internet_checksum(icmp, pkt->len()));
    std::memcpy(icmp + 2, &csum, 2);

    return ip_.transmit(pkt, IPProto::ICMP);
}
