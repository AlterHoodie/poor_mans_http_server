#include "ip6.h"
#include "buff.h"
#include "eth.h"
#include "handler.h"
#include "net_addr.h"
#include "utils.h"

#include <cstring>
#include <cstdint>

namespace {

constexpr size_t kIp6HeaderLen = 40;

}  // namespace

IP6Handler::IP6Handler(const ip6_addr_t& ip, EthernetHandler& below, NdpCache& cache)
    : ip_(ip), below_(below), ndp_cache_(cache) {}

void IP6Handler::handle_packet(pkt_buff* pkt) {
    ip_ver_t ip_version(pkt->data);
    if (!ip_version.is_ip6() || pkt->len() < kIp6HeaderLen) {
        return;
    }

    const ip6_addr_t dst_ip(pkt->data + 24);
    if (dst_ip != ip_) {
        return;
    }

    const uint16_t payload_len =
        (static_cast<uint16_t>(pkt->data[4]) << 8) | pkt->data[5];
    if (payload_len + kIp6HeaderLen > pkt->len()) {
        return;
    }
    pkt->tail = pkt->data + kIp6HeaderLen + payload_len;

    const ip6_addr_t src_ip(pkt->data + 8);
    if (!src_ip.is_unspecified()) {
        ndp_cache_.learn(src_ip, mac_addr_t(pkt->l2_src));
    }

    const uint8_t proto = pkt->data[6];
    ProtocolHandler* handler = get_handler(proto);
    if (!handler) {
        return;
    }

    pkt->is_v6 = true;
    std::memcpy(pkt->ip6_src, pkt->data + 8, 16);
    std::memcpy(pkt->ip6_dst, pkt->data + 24, 16);

    pull(pkt, kIp6HeaderLen);
    handler->handle_packet(pkt);
}

ssize_t IP6Handler::transmit(pkt_buff* pkt, IPProto proto) {
    const ip6_addr_t dst(pkt->ip6_src);

    auto mac = ndp_cache_.lookup(dst);
    if (!mac) {
        return -1;
    }

    push(pkt, kIp6HeaderLen);
    uint8_t* ip = pkt->data;

    ip[0] = 0x60;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 0;
    const uint16_t payload_len = htons(static_cast<uint16_t>(pkt->len() - kIp6HeaderLen));
    std::memcpy(ip + 4, &payload_len, 2);
    ip[6] = static_cast<uint8_t>(proto);
    ip[7] = 64;
    std::memcpy(ip + 8,  ip_.data(), 16);
    std::memcpy(ip + 24, dst.data(), 16);

    return below_.transmit(pkt, *mac, EtherType::IPv6);
}

void IP6Handler::register_protocol(uint8_t ip_proto, ProtocolHandler& proto) {
    ip_handler_reg_[ip_proto] = &proto;
}

ProtocolHandler* IP6Handler::get_handler(uint8_t ip_proto) {
    auto it = ip_handler_reg_.find(ip_proto);
    if (it == ip_handler_reg_.end()) {
        return nullptr;
    }
    return it->second;
}
