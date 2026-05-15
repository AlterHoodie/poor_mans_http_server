#include "eth.h"
#include "buff.h"
#include "handler.h"
#include "net_addr.h"
#include "utils.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <sys/types.h>
EthernetHandler::EthernetHandler(const mac_addr_t& eth_local_mac, Dpdk& below)
    : eth_local_mac_(eth_local_mac), below_(below)
{}

void EthernetHandler::register_protocol(uint16_t ether_type, ProtocolHandler& handler){
    eth_handler_reg_[ether_type] = &handler;
}

void EthernetHandler::handle_packet(pkt_buff* buff){
    /*
        Ethernet II Frame (Big Endian)
        -----------------
        Dest MAC   : 6 bytes
        Src MAC    : 6 bytes
        EtherType  : 2 bytes
    */

    // validate minimum header size
    if ((buff->len()) < ETH_HEADER_SIZE){
        return;
    }

    // current ethernet header view
    uint8_t* frame = buff->data;

    // field views
    uint8_t* dst_mac = frame;

    // avoid alignment issues:
    // manually reconstruct EtherType from bytes (EtherType present in 0 and 1 bytes)
    uint16_t ether_type =
        (static_cast<uint16_t>(frame[12]) << 8) |
         static_cast<uint16_t>(frame[13]);
    
    // validate destination mac
    mac_addr_t dst_mac_addr(std::span<const std::uint8_t, 6>(dst_mac, 6));
    if (!validate_dst_mac(dst_mac_addr)){
        return;
    }

    // validate supported protocol
    if (!validate_ether_type(ether_type)){
        return;
    }

    // consume ethernet header
    pull(buff, ETH_HEADER_SIZE);

    // dispatch upward
    ProtocolHandler* handler = get_handler(ether_type);

    if (!handler){
        return;
    }

    handler->handle_packet(buff);
}

ssize_t EthernetHandler::transmit(pkt_buff *buff, const mac_addr_t& dst_mac, EtherType ether_type){
    // Come back to eth header
    push(buff, ETH_HEADER_SIZE);
    std::memcpy(buff->data, dst_mac.data(), 6);
    std::memcpy(buff->data + 6, eth_local_mac_.data(), 6);

    const uint16_t et = htons(static_cast<uint16_t>(ether_type));


    std::memcpy(buff->data+12, &et, sizeof(ether_type));

    return below_.transmit(buff);
}


bool EthernetHandler::validate_dst_mac(const mac_addr_t& mac_addr){
    if (mac_addr == eth_local_mac_) {
        return true;
    }
    if (mac_addr == mac_addr_t::broadcast()) {
        return true;
    }
    return false;
}

bool EthernetHandler::validate_ether_type(uint16_t ether_type){
    switch(static_cast<EtherType>(ether_type)){
        case EtherType::ARP:
        case EtherType::IPv4:
        case EtherType::IPv6:
            return true;
    }
    return false;
}

ProtocolHandler *EthernetHandler::get_handler(uint16_t ether_type){
    auto it = eth_handler_reg_.find(ether_type);
    if (it != eth_handler_reg_.end()){
        return it->second;
    }
    return nullptr;
}