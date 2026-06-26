#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

#include "buff.h"
#include "ip.h"
#include "ip6.h"
#include "net_addr.h"
#include "tcp.h"
#include "utils.h"


TCPHandler::TCPHandler(IPHandler& ip4, IP6Handler& ip6)
    : ip4_(ip4), ip6_(ip6) {}

void TCPHandler::handle_syn_rcvd(uint8_t flags, uint32_t ack_num, TCPSocket &socket){
    if (!has_flag(flags, TCPFlags::ACK)) return;
    if (ack_num != socket.snd_nxt) return;

    socket.state = TCPState::ESTABLISHED;
    socket.snd_una = ack_num;

    if (socket.event_fd == -1) return;
    if (socket.listen_fd == -1) return;
    auto it = fd_table_.find(socket.listen_fd);
    if (it == fd_table_.end()) return;

    TCPSocket &listen_sock = it->second;

    if (!listen_sock.on_accept) return;
    listen_sock.on_accept(socket.event_fd);

    return;
}

void TCPHandler::handle_established(
    pkt_buff *pkt,
    uint8_t flags, 
    uint32_t ack_num,
    uint32_t seq_num,
    uint8_t t_header_len,
    TCPSocket &socket){
    
    if (has_flag(flags, TCPFlags::FIN)){
        socket.state = TCPState::CLOSE_WAIT;
        socket.rcv_nxt+=1;
        transmit(pkt, socket, TCPFlags::ACK);
        if(socket.on_close) socket.on_close();
        return;
    }
    if (has_flag(flags, TCPFlags::ACK)){
        if (ack_num > socket.snd_una) {
            uint32_t bytes_acked = ack_num - socket.snd_una;
            socket.snd_una = ack_num;
            while (bytes_acked-- > 0 && !socket.send_buf.data.empty())
                socket.send_buf.data.pop_front();
        }
    }

    size_t payload_len = pkt->len() - t_header_len;
    if(payload_len > 0){
        uint8_t *payload = pkt->data + t_header_len;

        if (seq_num == socket.rcv_nxt){
            socket.recv_buf.ready.insert(socket.recv_buf.ready.end(),payload, payload+payload_len);
            socket.rcv_nxt +=payload_len;

        while(true){
            auto it = socket.recv_buf.out_of_order.find(socket.rcv_nxt);
            if(it==socket.recv_buf.out_of_order.end()) break;
            socket.recv_buf.ready.insert(
                socket.recv_buf.ready.end(), it->second.begin(), it->second.end()
            );

            socket.rcv_nxt +=it->second.size();
            socket.recv_buf.out_of_order.erase(it);

        }

        if (socket.on_data) {
            socket.on_data(socket.recv_buf.ready.data(), socket.recv_buf.ready.size());
            socket.recv_buf.ready.clear();
        }
        
    }else if (seq_num > socket.rcv_nxt) {
        socket.recv_buf.out_of_order[seq_num] = std::vector<uint8_t>(payload, payload + payload_len);
    }
    transmit(pkt, socket, TCPFlags::ACK);
    }

    return;
}

void TCPHandler::handle_last_ack(
    ConnKey &key,
    uint8_t flags,
    uint32_t ack_num,
    TCPSocket &socket
){
    if(!has_flag(flags, TCPFlags::ACK)) return;
    if(ack_num != socket.snd_nxt) return;

    socket.snd_una = ack_num;
    socket.state = TCPState::CLOSED;
    auto it = conn_table_.find(key);
    int event_fd = it->second;
    conn_table_.erase(it);

    close(event_fd);
    auto it1 = fd_table_.find(event_fd);
    fd_table_.erase(it1);
}

void TCPHandler::handle_packet(pkt_buff* pkt) {
    if (pkt->len() < 20) return;

    uint16_t src_port = (uint16_t(pkt->data[0]) << 8) | pkt->data[1];
    uint16_t dst_port = (uint16_t(pkt->data[2]) << 8) | pkt->data[3];

    uint32_t seq_num = (uint32_t(pkt->data[4])  << 24) | (uint32_t(pkt->data[5])  << 16)
                     | (uint32_t(pkt->data[6])  <<  8) |  uint32_t(pkt->data[7]);
    uint32_t ack_num = (uint32_t(pkt->data[8])  << 24) | (uint32_t(pkt->data[9])  << 16)
                     | (uint32_t(pkt->data[10]) <<  8) |  uint32_t(pkt->data[11]);

    uint8_t t_header_len = (pkt->data[12] >> 4) * 4;
    if (t_header_len < 20 || t_header_len > pkt->len()) return;

    uint8_t flags = pkt->data[13];

    // Verify checksum using pseudo-header
    uint16_t orig_csum = (uint16_t(pkt->data[16]) << 8) | pkt->data[17];
    pkt->data[16] = 0;
    pkt->data[17] = 0;

    uint8_t pseudo[12];
    uint8_t pseudo6[40];
    if (pkt->is_v6) {
        build_ipv6_pseudo_header(pseudo6, pkt->ip6_src, pkt->ip6_dst,
                                 static_cast<uint32_t>(pkt->len()),
                                 static_cast<uint8_t>(IPProto::TCP));
        if (transport_checksum(pseudo6, 40, pkt->data, pkt->len()) != orig_csum) return;
    } else {
        build_ipv4_pseudo_header(pseudo, pkt->ip_src, pkt->ip_dst,
                                  static_cast<uint16_t>(pkt->len()),
                                  static_cast<uint8_t>(IPProto::TCP));
        if (transport_checksum(pseudo, 12, pkt->data, pkt->len()) != orig_csum) return;
    }

    // Restore checksum
    pkt->data[16] = orig_csum >> 8;
    pkt->data[17] = orig_csum & 0xFF;

    ConnKey key{};
    key.is_v6    = pkt->is_v6;
    key.src_port = src_port;
    key.dst_port = dst_port;
    if (pkt->is_v6) {
        key.src_ip6 = ip6_addr_t(pkt->ip6_src);
        key.dst_ip6 = ip6_addr_t(pkt->ip6_dst);
    } else {
        key.src_ip4 = ip4_addr_t(pkt->ip_src);
        key.dst_ip4 = ip4_addr_t(pkt->ip_dst);
    }

    // Existing connection
    auto it = conn_table_.find(key);
    if (it != conn_table_.end()) {
        TCPSocket &socket = fd_table_[it->second];

        switch (socket.state){
            case TCPState::SYN_RCVD: handle_syn_rcvd(
                flags,
                ack_num, 
                socket); return;
            case TCPState::ESTABLISHED: handle_established(
                pkt,
                flags,
                ack_num,
                seq_num, 
                t_header_len,
                socket); return;
            case TCPState::CLOSE_WAIT: return;
            case TCPState::LAST_ACK: handle_last_ack(
                key,
                flags,
                ack_num,
                socket); return;
            default: return;
        }
    }

    // New connection — must be a SYN
    if (!has_flag(flags, TCPFlags::SYN)) return;

    auto it1 = listen_table_.find(dst_port);
    if (it1 == listen_table_.end()) return;

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) return;

    uint32_t my_isn = 1000;

    TCPSocket socket{};
    socket.state            = TCPState::SYN_RCVD;
    socket.key              = key;
    socket.snd_una          = my_isn;
    socket.snd_nxt          = my_isn + 1;   // SYN-ACK consumes 1 seq number
    socket.rcv_nxt          = seq_num + 1;  // peer's SYN consumed 1
    socket.send_buf.base_seq = my_isn + 1;
    socket.recv_buf.base_seq = seq_num + 1;
    socket.event_fd         = efd;
    socket.listen_fd        = it1->second;

    conn_table_[key] = efd;
    fd_table_[efd]   = socket;

    transmit(pkt, socket, TCPFlags::SYN | TCPFlags::ACK);
}

ssize_t TCPHandler::transmit(pkt_buff* pkt, TCPSocket& socket, TCPFlags flags) {
    constexpr uint8_t  DATA_OFFSET = 5 << 4;   // 20-byte header, no options
    constexpr uint16_t WINDOW      = 65535;

    uint8_t pseudo[12];
    uint8_t pseudo6[40];
    auto&         send_data = socket.send_buf.data;

    auto write_tcp_header = [&]() {
        // Header only; payload is appended after pkt->tail.
        pkt->tail = pkt->data + 20;

        uint16_t sp = htons(socket.key.dst_port);
        uint16_t dp = htons(socket.key.src_port);
        std::memcpy(pkt->data,     &sp, 2);
        std::memcpy(pkt->data + 2, &dp, 2);

        uint32_t seq = htonl(socket.snd_una);
        uint32_t ack = htonl(socket.rcv_nxt);
        std::memcpy(pkt->data + 4, &seq, 4);
        std::memcpy(pkt->data + 8, &ack, 4);

        pkt->data[12] = DATA_OFFSET;
        pkt->data[13] = static_cast<uint8_t>(flags);

        uint16_t wnd = htons(WINDOW);
        std::memcpy(pkt->data + 14, &wnd, 2);

        pkt->data[16] = 0;
        pkt->data[17] = 0;
        pkt->data[18] = 0;
        pkt->data[19] = 0;
    };

    auto finalize_and_send = [&]() -> ssize_t {
        const uint16_t seg_len = static_cast<uint16_t>(pkt->tail - pkt->data);
        uint16_t csum = 0;
        if (pkt->is_v6) {
            build_ipv6_pseudo_header(pseudo6, pkt->ip6_dst, pkt->ip6_src,
                                     seg_len, static_cast<uint8_t>(IPProto::TCP));
            csum = htons(transport_checksum(pseudo6, 40, pkt->data, seg_len));
        } else {
            build_ipv4_pseudo_header(pseudo, pkt->ip_dst, pkt->ip_src, seg_len,
                                      static_cast<uint8_t>(IPProto::TCP));
            csum = htons(transport_checksum(pseudo, 12, pkt->data, seg_len));
        }
        std::memcpy(pkt->data + 16, &csum, 2);
        return pkt->is_v6 ? ip6_.transmit(pkt, IPProto::TCP)
                          : ip4_.transmit(pkt, IPProto::TCP);
    };

    // SYN/ACK, FIN-only, RST, pure ACK — no queued application data.
    if (send_data.empty()) {
        write_tcp_header();
        return finalize_and_send();
    }

    // One IP datagram per loop iteration with its own TCP header + seq advancement.
    ssize_t last_send = 0;
    while (!send_data.empty()) {
        write_tcp_header();

        const size_t tailroom = static_cast<size_t>(pkt->end - pkt->tail);
        const size_t payload_len = std::min(send_data.size(), tailroom);
        if (payload_len == 0) {
            return -1;
        }

        uint8_t* dst = pkt->tail;
        size_t   i = 0;
        for (uint8_t b : send_data) {
            if (i >= payload_len) break;
            dst[i++] = b;
        }
        pkt->tail += payload_len;

        socket.snd_nxt += static_cast<uint32_t>(payload_len);
        socket.snd_una = socket.snd_nxt;

        last_send = finalize_and_send();
        if (last_send < 0) return last_send;

        send_data.erase(send_data.begin(), send_data.begin() + static_cast<std::ptrdiff_t>(payload_len));
    }

    return last_send;
}


int TCPHandler::tcp_socket(){
    TCPSocket socket{};
    socket.state = TCPState::CLOSED;

    socket.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    fd_table_[socket.event_fd] = socket;

    return socket.event_fd;
}

int TCPHandler::tcp_bind(int fd, uint16_t port){
    auto it = fd_table_.find(fd);
    if(it == fd_table_.end()) throw std::runtime_error("Could Not find Socket to bind");

    it->second.key.dst_port = port;

    return 0;
}

ssize_t TCPHandler::tcp_send(int fd, const void *buf, size_t len){
    auto it = fd_table_.find(fd);
    if(it == fd_table_.end()) return -1;

    TCPSocket& socket = it->second;
    auto bytes = std::span<const uint8_t>(static_cast<const uint8_t*>(buf), len);

    auto& q = socket.send_buf.data;
    q.insert(q.end(), bytes.begin(), bytes.end());

    send_segment(socket, TCPFlags::PSH | TCPFlags::ACK);

    return static_cast<ssize_t>(len);
}   

ssize_t TCPHandler::send_segment(TCPSocket& socket, TCPFlags flags){
    auto buff_u = create_buffer();
    pkt_buff* pkt = buff_u.get();

    pkt->is_v6 = socket.key.is_v6;
    pkt->data = pkt->head + (pkt->is_v6 ? 54 : 34);
    pkt->tail = pkt->data;

    if (pkt->is_v6) {
        std::memcpy(pkt->ip6_src, socket.key.src_ip6.data(), 16);
        std::memcpy(pkt->ip6_dst, socket.key.dst_ip6.data(), 16);
    } else {
        std::memcpy(pkt->ip_src, socket.key.src_ip4.data(), 4);
        std::memcpy(pkt->ip_dst, socket.key.dst_ip4.data(), 4);
    }

    ssize_t r = transmit(pkt, socket, flags);

    return r;
}

int TCPHandler::tcp_close(int fd){
    auto it = fd_table_.find(fd);
    if(it==fd_table_.end()) return -1;

    TCPSocket& socket = it->second;
    if (socket.state == TCPState::LISTEN ){
        listen_table_.erase(socket.key.dst_port);
        close(socket.event_fd);
        fd_table_.erase(fd);
        return 0;
    }
    if (socket.state == TCPState::ESTABLISHED){
        socket.snd_nxt+=1;
        socket.state = TCPState::FIN_WAIT_1;
        const ssize_t n = send_segment(socket, TCPFlags::FIN | TCPFlags::ACK);
        return n>=0? 0:-1;
    }
    if (socket.state == TCPState::CLOSE_WAIT){
        socket.snd_nxt+=1;
        socket.state = TCPState::LAST_ACK;
        const ssize_t n = send_segment(socket, TCPFlags::FIN | TCPFlags::ACK);
        return n>=0? 0:-1;
    }

    return -1;
}

int TCPHandler::tcp_listen(int fd){
    auto it1 = fd_table_.find(fd);
    if(it1 == fd_table_.end()) return -1;
    
    TCPSocket& socket = it1->second;
    socket.state = TCPState::LISTEN;
    listen_table_[socket.key.dst_port] = fd;

    return 0;
}

void TCPHandler::set_on_accept(int fd, std::function<void(const int fd)> cb){
    auto it = fd_table_.find(fd);
    if(it == fd_table_.end()) return;
    it->second.on_accept = cb;
}

void TCPHandler::set_on_data(int fd, std::function<void(const uint8_t*, size_t)> foo){
    auto it = fd_table_.find(fd);
    if(it == fd_table_.end()) return;

    it->second.on_data = foo;
}

void TCPHandler::set_on_close(int fd, std::function<void()> foo){
    auto it = fd_table_.find(fd);
    if(it == fd_table_.end()) return;

    it->second.on_close = foo;
}