#include "icmp6.h"
#include "buff.h"
#include "ip.h"
#include "utils.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr size_t kIcmp6MinLen = 8;
constexpr size_t kNdTargetOff = 8;
constexpr size_t kNdOptionsOff = 24;

void icmp6_checksum(pkt_buff* pkt, uint8_t* icmp, size_t icmp_len) {
    icmp[2] = 0;
    icmp[3] = 0;

    uint8_t pseudo[40];
    build_ipv6_pseudo_header(pseudo, pkt->ip6_src, pkt->ip6_dst,
                             static_cast<uint32_t>(icmp_len),
                             static_cast<uint8_t>(IPProto::ICMPv6));
    const uint16_t csum = htons(
        transport_checksum(pseudo, 40, icmp, icmp_len));
    std::memcpy(icmp + 2, &csum, 2);
}

}  // namespace

ICMP6Handler::ICMP6Handler(const mac_addr_t& mac, const ip6_addr_t& ip,
                           IP6Handler& ip6, NdpCache& cache)
    : ip6_(ip6), ndp_cache_(cache), ip_(ip), mac_(mac) {}

void ICMP6Handler::learn_options(const uint8_t* icmp, size_t len,
                                 const ip6_addr_t& ip) {
    if (len < kNdOptionsOff) return;

    size_t off = kNdOptionsOff;
    while (off + 2 <= len) {
        const uint8_t opt_type = icmp[off];
        const uint8_t opt_len_units = icmp[off + 1];
        if (opt_len_units == 0) break;
        const size_t opt_len = static_cast<size_t>(opt_len_units) * 8;
        if (off + opt_len > len) break;

        if ((opt_type == 1 || opt_type == 2) && opt_len >= 8) {
            ndp_cache_.learn(ip, mac_addr_t(icmp + off + 2));
        }
        off += opt_len;
    }
}

ssize_t ICMP6Handler::send_neighbor_advertisement(pkt_buff* pkt,
                                                  const ip6_addr_t& target) {
    constexpr size_t kNaLen = 32;
    pkt->tail = pkt->data + kNaLen;

    uint8_t* icmp = pkt->data;
    icmp[0] = static_cast<uint8_t>(ICMP6Type::NeighborAdvertisement);
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    icmp[4] = 0;
    icmp[5] = 0;
    icmp[6] = 0x40;  // Solicited flag
    icmp[7] = 0;
    std::memcpy(icmp + kNdTargetOff, target.data(), 16);
    icmp[24] = 2;  // Target Link-Layer Address
    icmp[25] = 1;  // length in units of 8 octets
    std::memcpy(icmp + 26, mac_.data(), 6);

    icmp6_checksum(pkt, icmp, kNaLen);
    return ip6_.transmit(pkt, IPProto::ICMPv6);
}

void ICMP6Handler::handle_packet(pkt_buff* pkt) {
    if (pkt->len() < kIcmp6MinLen) {
        return;
    }

    uint8_t* icmp = pkt->data;
    const uint8_t type = icmp[0];

    if (type == static_cast<uint8_t>(ICMP6Type::NeighborSolicitation)) {
        if (pkt->len() < kNdOptionsOff) return;

        const ip6_addr_t target(icmp + kNdTargetOff);
        learn_options(icmp, pkt->len(), ip6_addr_t(pkt->ip6_src));

        if (target != ip_) return;
        send_neighbor_advertisement(pkt, target);
        return;
    }

    if (type == static_cast<uint8_t>(ICMP6Type::NeighborAdvertisement)) {
        const ip6_addr_t target(icmp + kNdTargetOff);
        learn_options(icmp, pkt->len(), target);
        return;
    }

    if (type != static_cast<uint8_t>(ICMP6Type::EchoRequest) || icmp[1] != 0) {
        return;
    }

    transmit_echo_reply(pkt);
}

ssize_t ICMP6Handler::transmit_echo_reply(pkt_buff* pkt) {
    uint8_t* icmp = pkt->data;
    icmp[0] = static_cast<uint8_t>(ICMP6Type::EchoReply);
    icmp6_checksum(pkt, icmp, pkt->len());
    return ip6_.transmit(pkt, IPProto::ICMPv6);
}
