#include "udp.h"
#include "buff.h"
#include "ip.h"
#include "ip6.h"
#include "tick.h"
#include "tick_handler.h"
#include "utils.h"

#include <cstdint>
#include <cstring>
#include <sys/types.h>

UDPHandler::UDPHandler(IPHandler& ip4, IP6Handler& ip6)
    : ip4_(ip4), ip6_(ip6) {}

void UDPHandler::handle_packet(pkt_buff* pkt) {
    constexpr size_t kUdpHeaderLen = 8;
    constexpr size_t kTickDatagramLen = kUdpHeaderLen + sizeof(Tick);

    if (pkt->len() < kUdpHeaderLen) {
        ++hdr_drop;
        return;
    }

    const uint16_t src_port = (uint16_t(pkt->data[0]) << 8) | pkt->data[1];
    const uint16_t dst_port = (uint16_t(pkt->data[2]) << 8) | pkt->data[3];
    const uint16_t udp_len  = (uint16_t(pkt->data[4]) << 8) | pkt->data[5];

    if (udp_len != kTickDatagramLen || udp_len > pkt->len()) {
        ++hdr_drop;
        return;
    }

    pkt->tail = pkt->data + udp_len;

    if (bind_table_.find(dst_port) == bind_table_.end()) return;

    handle_tick(*reinterpret_cast<const Tick*>(pkt->data + kUdpHeaderLen),
                stats, work_iters_);

    if (!echo_) return;

    uint16_t sp = htons(dst_port);
    uint16_t dp = htons(src_port);
    std::memcpy(pkt->data,     &sp, 2);
    std::memcpy(pkt->data + 2, &dp, 2);

    transmit(pkt);
}

ssize_t UDPHandler::transmit(pkt_buff* pkt) {
    pkt->data[6] = 0;
    pkt->data[7] = 0;

    const uint16_t udp_len = static_cast<uint16_t>(pkt->len());
    uint16_t len_be = htons(udp_len);
    std::memcpy(pkt->data + 4, &len_be, 2);

    if (pkt->is_v6) {
        uint8_t pseudo[40];
        build_ipv6_pseudo_header(pseudo, pkt->ip6_dst, pkt->ip6_src,
                                 udp_len, static_cast<uint8_t>(IPProto::UDP));
        const uint16_t csum = htons(
            transport_checksum(pseudo, 40, pkt->data, pkt->len()));
        std::memcpy(pkt->data + 6, &csum, 2);
        return ip6_.transmit(pkt, IPProto::UDP);
    }

    uint8_t pseudo[12];
    build_ipv4_pseudo_header(pseudo, pkt->ip_dst, pkt->ip_src, udp_len,
                              static_cast<uint8_t>(IPProto::UDP));
    const uint16_t csum = htons(
        transport_checksum(pseudo, 12, pkt->data, pkt->len()));
    std::memcpy(pkt->data + 6, &csum, 2);

    return ip4_.transmit(pkt, IPProto::UDP);
}

void UDPHandler::udp_bind(uint16_t port) {
    bind_table_.insert(port);
}

void UDPHandler::set_echo(bool echo) {
    echo_ = echo;
}

void UDPHandler::set_work_iters(int work_iters) {
    work_iters_ = work_iters > 0 ? work_iters : 0;
}
