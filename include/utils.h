#include <cstdint>
#include <sys/types.h>

uint16_t ntohs(uint16_t x);
uint16_t htons(uint16_t x);

uint32_t ntohl(uint32_t x);
uint32_t htonl(uint32_t x);

void print_mac(const uint8_t *x);
void print_ether(uint8_t *dst_mac, uint8_t *src_mac, uint16_t ether_type);

// One's-complement Internet checksum (IPv4 header, full ICMP). See implementation for algorithm.
uint16_t internet_checksum(const uint8_t* data, size_t len);