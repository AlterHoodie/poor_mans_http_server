#include <bit>

#include "utils.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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

uint16_t transport_checksum(const uint8_t* pseudo, size_t pseudo_len,
                            const uint8_t* segment, size_t seg_len)
{
    uint32_t sum = 0;

    auto accumulate = [&](const uint8_t* buf, size_t len) {
        while (len > 1) {
            sum += (static_cast<uint32_t>(buf[0]) << 8) | buf[1];
            buf += 2;
            len -= 2;
        }
        if (len == 1) sum += static_cast<uint32_t>(buf[0]) << 8;
    };

    accumulate(pseudo, pseudo_len);
    accumulate(segment, seg_len);

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

void build_ipv4_pseudo_header(uint8_t out[12], const uint8_t* src_ip,
                              const uint8_t* dst_ip, uint16_t seg_len,
                              uint8_t proto)
{
    std::memcpy(out, src_ip, 4);
    std::memcpy(out + 4, dst_ip, 4);
    out[8] = 0;
    out[9] = proto;
    const uint16_t len_be = htons(seg_len);
    std::memcpy(out + 10, &len_be, 2);
}

void build_ipv6_pseudo_header(uint8_t out[40], const uint8_t* src_ip,
                              const uint8_t* dst_ip, uint32_t seg_len,
                              uint8_t proto)
{
    std::memcpy(out, src_ip, 16);
    std::memcpy(out + 16, dst_ip, 16);
    out[32] = static_cast<uint8_t>((seg_len >> 24) & 0xFF);
    out[33] = static_cast<uint8_t>((seg_len >> 16) & 0xFF);
    out[34] = static_cast<uint8_t>((seg_len >> 8) & 0xFF);
    out[35] = static_cast<uint8_t>(seg_len & 0xFF);
    out[36] = 0;
    out[37] = 0;
    out[38] = 0;
    out[39] = proto;
}