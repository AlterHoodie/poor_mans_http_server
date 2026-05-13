#include "ip.h"
#include "buff.h"
#include "eth.h"
#include "handler.h"
#include "net_addr.h"
#include "utils.h"

#include <cstring>
#include <cstdint>
#include <sys/types.h>

IPHandler::IPHandler(const ip4_addr_t& ip, EthernetHandler& below, ArpCache& cache)
    : ip_(ip), below_(below), arp_cache_(cache) {}

void IPHandler::handle_packet(pkt_buff* pkt) {
    ip_ver_t ip_version(pkt->data);

    if (ip_version.is_ip6()) {
        return;
    }

    uint8_t ihl = *pkt->data & 0x0F;
    if (ihl < 5 || ihl > 15) {
        return;
    }

    uint8_t ip_header_len = ihl * 4;
    if (ip_header_len > pkt->len()) {
        return;
    }

    // validate destination — drop packets not addressed to us
    const ip4_addr_t dst_ip(pkt->data + 16);
    if (dst_ip != ip_) {
        return;
    }

    // verify checksum: zero field, recompute, compare
    uint8_t* ip_csum = pkt->data + 10;
    const uint16_t orig_csum = (static_cast<uint16_t>(ip_csum[0]) << 8) | ip_csum[1];
    ip_csum[0] = 0;
    ip_csum[1] = 0;
    const uint16_t computed = internet_checksum(pkt->data, static_cast<size_t>(ip_header_len));
    if (computed != orig_csum) {
        return;
    }
    // restore checksum so upper layers can access the intact header if needed
    ip_csum[0] = orig_csum >> 8;
    ip_csum[1] = orig_csum & 0xFF;

    uint8_t proto = pkt->data[9];
    ProtocolHandler* handler = get_handler(proto);
    if (!handler) {
        return;
    }

    std::memcpy(pkt->ip_src, pkt->data + 12, 4);
    std::memcpy(pkt->ip_dst, pkt->data + 16, 4);

    pull(pkt, ip_header_len);
    handler->handle_packet(pkt);
}

ssize_t IPHandler::transmit(pkt_buff* pkt, uint8_t proto) {
    constexpr uint8_t IHL        = 5;
    constexpr uint8_t IP_HDR_LEN = IHL * 4;  // 20

    const ip4_addr_t dst(pkt->ip_src);  // reply always goes back to the sender

    auto mac = arp_cache_.lookup(dst);
    if (!mac) {
        return -1;  // no ARP entry for this destination yet
    }

    push(pkt, IP_HDR_LEN);
    uint8_t* ip = pkt->data;

    ip[0] = (4 << 4) | IHL;                                         // version=4, IHL=5
    ip[1] = 0;                                                       // DSCP/ECN
    const uint16_t total_len = htons(static_cast<uint16_t>(pkt->len()));
    std::memcpy(ip + 2, &total_len, 2);
    ip[4] = ip[5] = 0;                                               // identification
    ip[6] = ip[7] = 0;                                               // flags + fragment offset
    ip[8] = 64;                                                      // TTL
    ip[9] = proto;
    ip[10] = ip[11] = 0;                                             // checksum = 0 before compute
    std::memcpy(ip + 12, ip_.data(),  4);                            // src = our IP
    std::memcpy(ip + 16, dst.data(),  4);                            // dst = original sender

    const uint16_t csum = htons(internet_checksum(ip, IP_HDR_LEN));
    std::memcpy(ip + 10, &csum, 2);

    return below_.transmit(pkt, *mac, EtherType::IPv4);
}

void IPHandler::register_protocol(uint8_t ip_proto, ProtocolHandler& proto) {
    ip_handler_reg_[ip_proto] = &proto;
}

ProtocolHandler* IPHandler::get_handler(uint8_t ip_proto) {
    auto it = ip_handler_reg_.find(ip_proto);
    if (it == ip_handler_reg_.end()) {
        return nullptr;
    }
    return it->second;
}
