#include <bit>

#include "utils.h"
#include <cstddef>
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

/*
 * Internet checksum (RFC 1071 style): used by IPv4 (header only) and ICMP (whole message).
 *
 * Why it exists: on the wire, bits get flipped or lengths get sliced wrong. A checksum lets
 * receivers discard obviously corrupted datagrams without trusting every byte. It is cheap in
 * software (adds and shifts), but weak compared to a CRC—good enough for the historical IP
 * design, not a security or integrity guarantee.
 *
 * How it works:
 *   - Treat the buffer as 16-bit big-endian words and add them using 32-bit math so carries
 *     from the low 16 bits can be folded back in.
 *   - "Fold" carries: any overflow past 16 bits is added back into the low 16 bits until the
 *     high half is zero—this is one's-complement addition semantics.
 *   - Take one's complement (~) of the folded 16-bit sum; that value is the checksum field.
 *   - To *verify*, include the received checksum in the same sum; a correct packet folds to
 *     0xFFFF (all ones in one's-complement terms).
 *
 * Callers must set the checksum field in the buffer to 0 before computing the value to store.
 * We build each word as (hi << 8) | lo so the sum matches the on-the-wire byte order on any
 * host endianness (do not cast to uint16_t* on little-endian and sum those values).
 */
uint16_t internet_checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;

    while (len > 1) {
        sum += (static_cast<uint32_t>(data[0]) << 8) | data[1];
        data += 2;
        len -= 2;
    }

    // Odd final byte: pad with an implicit zero high byte (may happen for some ICMP lengths).
    if (len == 1) {
        sum += static_cast<uint32_t>(data[0]) << 8;
    }

    // Fold overflow into low 16 bits until no overflow remains.
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}