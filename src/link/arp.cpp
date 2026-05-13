#include "arp.h"
#include "buff.h"
#include "eth.h"
#include "net_addr.h"

#include <cstdint>
#include <cstring>
#include <span>

namespace {

constexpr std::uint16_t kArpHwEther = 1;
constexpr std::uint16_t kArpProtoIPv4 = 0x0800;
constexpr std::uint8_t kArpHwAddrLenEth = 6;
constexpr std::uint8_t kArpProtoLenIPv4 = 4;
constexpr std::uint16_t kArpOpRequest = 1;
constexpr std::size_t kArpEthIPv4Pdu = 28;

}  // namespace

ARPHandler::ARPHandler(const mac_addr_t& mac, const ip4_addr_t& ip, EthernetHandler& eth, ArpCache& cache)
    : mac_(mac), ip_(ip), eth_(eth), arp_cache_(cache) {}

void ARPHandler::handle_packet(pkt_buff* pkt) {
    if (pkt->len() < kArpEthIPv4Pdu) {
        return;
    }

    std::uint8_t* arp = pkt->data;

    const std::uint16_t hw_type =
        (static_cast<std::uint16_t>(arp[0]) << 8) | arp[1];
    const std::uint16_t proto_type =
        (static_cast<std::uint16_t>(arp[2]) << 8) | arp[3];
    if (hw_type != kArpHwEther || proto_type != kArpProtoIPv4 ||
        arp[4] != kArpHwAddrLenEth || arp[5] != kArpProtoLenIPv4) {
        return;
    }

    // learn sender's IP->MAC regardless of op (useful for both requests and replies)
    arp_cache_.learn(ip4_addr_t(std::span<const std::uint8_t, 4>(arp + 14, 4)),
                     mac_addr_t(std::span<const std::uint8_t, 6>(arp + 8,  6)));

    const std::uint16_t op =
        (static_cast<std::uint16_t>(arp[6]) << 8) | arp[7];
    if (op != kArpOpRequest) {
        return;
    }

    const ip4_addr_t target_ip(std::span<const std::uint8_t, 4>(arp + 24, 4));
    if (target_ip != ip_) {
        return;
    }

    transmit(pkt);
}

ssize_t ARPHandler::transmit(pkt_buff *pkt){
    uint8_t *arp = pkt->data;

    std::uint8_t op_reply[2] = {0x00, 0x02};
    std::memcpy(arp + 6, op_reply, 2);

    std::uint8_t* req_src_mac = arp + 8;
    std::uint8_t* req_dst_mac = arp + 18;

    mac_addr_t temp_mac(req_src_mac);

    std::memcpy(req_src_mac, mac_.data(), 6);
    std::memcpy(req_dst_mac, temp_mac.data(), 6);

    std::uint8_t* req_src_ip = arp + 14;
    std::uint8_t* req_dst_ip = arp + 24;
    ip4_addr_t temp_ip(req_src_ip);
    
    std::memcpy(req_src_ip, ip_.data(), 4);
    std::memcpy(req_dst_ip, temp_ip.data(), 4);


    return eth_.transmit(pkt, temp_mac, EtherType::ARP);
}