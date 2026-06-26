#include <cstddef>
#include <cstdint>
#include <sys/types.h>

uint16_t ntohs(uint16_t x);
uint16_t htons(uint16_t x);

uint32_t ntohl(uint32_t x);
uint32_t htonl(uint32_t x);

void print_mac(const uint8_t *x);
// One's-complement Internet checksum (IPv4 header, full ICMP). See implementation for algorithm.
uint16_t internet_checksum(const uint8_t* data, size_t len);

// TCP/UDP/ICMPv6 over IPv4 pseudo-header (12 bytes) or IPv6 pseudo-header (40 bytes).
uint16_t transport_checksum(const uint8_t* pseudo, size_t pseudo_len,
                            const uint8_t* segment, size_t seg_len);

void build_ipv4_pseudo_header(uint8_t out[12], const uint8_t* src_ip,
                              const uint8_t* dst_ip, uint16_t seg_len,
                              uint8_t proto);

void build_ipv6_pseudo_header(uint8_t out[40], const uint8_t* src_ip,
                              const uint8_t* dst_ip, uint32_t seg_len,
                              uint8_t proto);