#include <bit>

#include "utils.h"
#include <cstdint>
#include <iomanip>
#include <iostream>

uint16_t ntohs(uint16_t x){
    // Compiler pre computes/optimizes branch selection , so we dont get lose cycles computing this branch
    if constexpr (std::endian::native == std::endian::big){
        return x;
    }
    return (x >> 8 | x << 8);
}

uint16_t htons(uint16_t x){
    return ntohs(x);
}

uint32_t ntohl(uint32_t x){
    // Compiler pre computes/optimizes branch selection , so we dont get lose cycles computing this branch 
    if constexpr (std::endian::native == std::endian::big){
        return x;
    }
    return (
        ((x >> 24) & 0x000000FF)|
        ((x >> 8)  & 0x0000FF00) |
        ((x << 8)  & 0x00FF0000) |
        ((x << 24) & 0xFF000000)
    );
}

uint32_t htonl(uint32_t x){
    return ntohl(x);
}

void print_mac(const uint8_t *x){
    std::cout << "Mac Addr: ";
    for (int i = 0; i < 6; ++i) {
        if (i != 0) {
            std::cout << ':';
        }
        std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                  << static_cast<unsigned>(x[i]);
    }
    std::cout << std::dec << std::setfill(' ') << std::setw(0) << '\n';
}

void print_ether(uint8_t *dst_mac, uint8_t *src_mac, uint16_t ether_type){
    printf("--------------------------\n");
    std::cout<< "Recieved Ethernet Frame\n";
    std::cout<<"Src ";
    print_mac(static_cast<const uint8_t*>(src_mac));
    std::cout<<"Dst ";
    print_mac(static_cast<const uint8_t*>(dst_mac));
    
    printf("Ether Type: %x\n", ether_type);
    printf("--------------------------\n");
}