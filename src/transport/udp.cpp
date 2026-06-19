#include "udp.h"
#include "buff.h"
#include "ip.h"
#include "tick.h"
#include "tick_handler.h"
#include "utils.h"

#include <cstdint>
#include <cstring>
#include <sys/types.h>

// Builds a 12-byte IPv4 UDP pseudo-header for checksum computation.
static void build_udp_pseudo_header(uint8_t out[12],
                                    const uint8_t* src_ip,
                                    const uint8_t* dst_ip,
                                    uint16_t        udp_len)
{
    std::memcpy(out,     src_ip, 4);
    std::memcpy(out + 4, dst_ip, 4);
    out[8]  = 0;
    out[9]  = static_cast<uint8_t>(IPProto::UDP);
    uint16_t len_be = htons(udp_len);
    std::memcpy(out + 10, &len_be, 2);
}

// One's-complement checksum over two non-contiguous buffers.
static uint16_t udp_checksum(const uint8_t* pseudo, size_t pseudo_len,
                              const uint8_t* segment, size_t seg_len)
{
    uint32_t sum = 0;

    auto accumulate = [&](const uint8_t* buf, size_t len) {
        while (len > 1) {
            sum += (uint32_t(buf[0]) << 8) | buf[1];
            buf += 2;
            len -= 2;
        }
        if (len == 1) sum += uint32_t(buf[0]) << 8;
    };

    accumulate(pseudo,  pseudo_len);
    accumulate(segment, seg_len);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return static_cast<uint16_t>(~sum);
}


UDPHandler::UDPHandler(IPHandler& ip) : ip_(ip) {}

void UDPHandler::handle_packet(pkt_buff* pkt) {
    /*
        UDP header layout (RFC 768)
        ---------------------------
        Source Port      : 2 bytes [0-1]
        Destination Port : 2 bytes [2-3]
        Length           : 2 bytes [4-5]   (header + payload, min 8)
        Checksum         : 2 bytes [6-7]
        Payload          : variable [8+]
    */
    constexpr size_t kUdpHeaderLen = 8;
    if (pkt->len() < kUdpHeaderLen) return;

    const uint16_t src_port = (uint16_t(pkt->data[0]) << 8) | pkt->data[1];
    const uint16_t dst_port = (uint16_t(pkt->data[2]) << 8) | pkt->data[3];
    const uint16_t udp_len  = (uint16_t(pkt->data[4]) << 8) | pkt->data[5];
    const uint16_t orig_csum = (uint16_t(pkt->data[6]) << 8) | pkt->data[7];

    if (udp_len < kUdpHeaderLen || udp_len > pkt->len()) return;

    // Trim any trailing padding set by the IP layer.
    pkt->tail = pkt->data + udp_len;

    // Validate checksum when sender provided one (0 means omitted per RFC 768).
    if (orig_csum != 0) {
        pkt->data[6] = 0;
        pkt->data[7] = 0;
        uint8_t pseudo[12];
        build_udp_pseudo_header(pseudo, pkt->ip_src, pkt->ip_dst, udp_len);
        const uint16_t computed = udp_checksum(pseudo, 12, pkt->data, pkt->len());
        pkt->data[6] = orig_csum >> 8;
        pkt->data[7] = orig_csum & 0xFF;
        if (computed != orig_csum) return;
    }

    if (bind_table_.find(dst_port) == bind_table_.end()) return;

    const uint8_t* payload   = pkt->data + kUdpHeaderLen;
    const size_t   payload_len = pkt->len() - kUdpHeaderLen;

    if (payload_len == sizeof(Tick)) {
        Tick tick;
        std::memcpy(&tick, payload, sizeof(Tick));
        handle_tick(tick, stats, work_iters_);
    } else {
        ++bad_size;
    }

    if (!echo_) return;

    // Swap ports for the reply.
    uint16_t sp = htons(dst_port);
    uint16_t dp = htons(src_port);
    std::memcpy(pkt->data,     &sp, 2);
    std::memcpy(pkt->data + 2, &dp, 2);

    transmit(pkt);
}

ssize_t UDPHandler::transmit(pkt_buff* pkt) {
    // Recompute checksum (zero field first).
    pkt->data[6] = 0;
    pkt->data[7] = 0;

    const uint16_t udp_len = static_cast<uint16_t>(pkt->len());
    uint16_t len_be = htons(udp_len);
    std::memcpy(pkt->data + 4, &len_be, 2);

    uint8_t pseudo[12];
    // ip_src / ip_dst still hold the *incoming* addresses; IP layer will reply to ip_src.
    build_udp_pseudo_header(pseudo, pkt->ip_dst, pkt->ip_src, udp_len);
    const uint16_t csum = htons(udp_checksum(pseudo, 12, pkt->data, pkt->len()));
    std::memcpy(pkt->data + 6, &csum, 2);

    return ip_.transmit(pkt, IPProto::UDP);
}

void UDPHandler::udp_bind(uint16_t port) {
    bind_table_.insert(port);
}

void UDPHandler::set_echo(bool echo) {
    echo_ = echo;
}

void UDPHandler::set_work_iters(int work_iters) {
    work_iters_ = work_iters > 0 ? work_iters : 0;
}
