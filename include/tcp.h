#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <queue>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include "buff.h"
#include "handler.h"
#include "ip.h"
#include "net_addr.h"


struct ConnKey {
    ip4_addr_t src_ip, dst_ip;
    uint16_t   src_port, dst_port;

    bool operator==(const ConnKey&) const = default;
};

namespace std {
    template <>
    struct hash<ConnKey> {
        size_t operator()(const ConnKey& k) const noexcept {
            size_t h = 0;
            for (uint8_t b : k.src_ip.bytes) h = h * 131u + b;
            for (uint8_t b : k.dst_ip.bytes) h = h * 131u + b;
            h = h * 131u + k.src_port;
            h = h * 131u + k.dst_port;
            return h;
        }
    };
}

enum class TCPState {
    CLOSED,
    LISTEN,
    SYN_SENT,
    SYN_RCVD,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    TIME_WAIT,
    CLOSE_WAIT,
    LAST_ACK,
    CLOSING,
};

enum class TCPFlags : uint8_t {
    FIN = 0x01,
    SYN = 0x02,
    RST = 0x04,
    PSH = 0x08,
    ACK = 0x10,
    URG = 0x20,
    ECE = 0x40,
    CWR = 0x80,
};

constexpr TCPFlags operator|(TCPFlags a, TCPFlags b) noexcept {
    return static_cast<TCPFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool has_flag(uint8_t flags, TCPFlags f) noexcept {
    return (flags & static_cast<uint8_t>(f)) != 0;
}

struct SendBuff {
    std::deque<uint8_t> data;
    uint32_t            base_seq{0};
};

struct RecvBuff {
    std::map<uint32_t, std::vector<uint8_t>> out_of_order;
    std::deque<uint8_t>                      ready;
    uint32_t                                 base_seq{0};
};

struct TCPSocket {
    ConnKey  key;
    TCPState state{TCPState::CLOSED};

    uint32_t snd_una{0};
    uint32_t snd_nxt{0};
    uint32_t rcv_nxt{0};

    SendBuff send_buf;
    RecvBuff recv_buf;

    std::queue<int> accept_queue;
    int             event_fd{-1};
    int             listen_fd{-1};  // parent listen socket fd (-1 if not a child)
};


class TCPHandler : public ProtocolHandler {
private:
    IPHandler& below_;

    std::unordered_map<ConnKey,   int>       conn_table_;
    std::unordered_map<int,       TCPSocket> fd_table_;
    std::unordered_map<uint16_t,  int>       listen_table_;

    void handle_syn_rcvd(uint8_t flags, uint32_t ack_num, TCPSocket &socket);
    void handle_established(
        pkt_buff *pkt, 
        uint8_t flags, 
        uint32_t ack_num,
        uint32_t seq_num,
        uint8_t t_header_len,
        TCPSocket &socket);
    void handle_last_ack( 
        ConnKey &key,
        uint8_t flags,
        uint32_t ack_num,
        TCPSocket &socket);
    ssize_t send_segment(
        TCPSocket& socket,
        TCPFlags flags
    );

public:
    explicit TCPHandler(IPHandler& below);
    ~TCPHandler() = default;

    void    handle_packet(pkt_buff* pkt) override;
    ssize_t transmit(pkt_buff* pkt, TCPSocket& socket, TCPFlags flags);

    int     tcp_socket();
    int     tcp_bind(int fd, ip4_addr_t ip, uint16_t port);
    int     tcp_listen(int fd, int backlog);
    int     tcp_accept(int fd);                        // -1 = queue empty
    ssize_t tcp_send(int fd, const void* buf, size_t len);
    // recv: >0 bytes; 0 = FIN and queue drained; -1 + EAGAIN = no payload yet; -1 + EBADF bad fd
    ssize_t tcp_recv(int fd, void* buf, size_t len);
    int     tcp_close(int fd);
};
