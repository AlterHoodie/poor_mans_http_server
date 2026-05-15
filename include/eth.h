#pragma once

#include <cstdint>
#include <unordered_map>

#include "buff.h"
#include "dpdk.h"
#include "handler.h"
#include "net_addr.h"


enum class EtherType : std::uint16_t {
    IPv4 = 0x0800,
    ARP  = 0x0806,
    IPv6 = 0x86DD,
};

constexpr size_t ETH_HEADER_SIZE = 14;

class EthernetHandler:public ProtocolHandler{
    private:
        mac_addr_t eth_local_mac_;
        std::unordered_map<uint16_t, ProtocolHandler*> eth_handler_reg_;
        Dpdk& below_;

    public:
        EthernetHandler(const mac_addr_t& eth_local_mac, Dpdk& below);
        ~EthernetHandler() = default;

        void handle_packet(pkt_buff* buff) override;
        ssize_t transmit(pkt_buff* buff, const mac_addr_t &dst_mac, EtherType ether_type);
        void register_protocol(uint16_t ether_type, ProtocolHandler& handler);

        bool validate_dst_mac(const mac_addr_t& mac_addr);
        bool validate_ether_type(uint16_t ether_type);

        ProtocolHandler* get_handler(uint16_t ether_type);
        
};