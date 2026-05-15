#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unordered_map>

#include "arp.h"
#include "arp_cache.h"
#include "buff.h"
#include "eth.h"
#include "event_loop.h"
#include "http_types.h"
#include "icmp.h"
#include "ip.h"
#include "net_addr.h"
#include "parser.h"
#include "request.h"
#include "response.h"
#include "tap.h"
#include "tcp.h"
#include "utils.h"
#include "router.h"

void pump_connection(HTTPConnection &conn, Router &router){
    if (conn.state == HTTPState::WRITING || conn.state == HTTPState::CLOSED) return;

    if (conn.state == HTTPState::READING_HEADERS){
        size_t header_end = conn.read_buf.find("\r\n\r\n");
        if (header_end == std::string::npos){
            return;
        }
        conn.body_start = header_end + 4;
        Request hdr = parse_request(conn.read_buf.substr(0, conn.body_start));
        conn.content_length = parse_content_length(hdr);
        conn.state = conn.content_length > 0 ? HTTPState::READING_BODY : HTTPState::PROCESSING;
    }

    if (conn.state == HTTPState::READING_BODY){
        if(conn.read_buf.size() < conn.body_start + conn.content_length) return;

        conn.state = HTTPState::PROCESSING;
    }
    if (conn.state == HTTPState::PROCESSING){
        Request req = parse_request(conn.read_buf);
        Response res = router.route(req);

        conn.write_buf = build_response_string(res);
        conn.read_buf.clear();
        conn.state = HTTPState::WRITING;
    }
}

int main(){
    EventLoop loop = EventLoop();
    Tap tap = Tap("tap0");
    int tapfd = tap.fd();
    const uint8_t* x = tap.mac();
    mac_addr_t tap_mac(x);
    ip4_addr_t ip(192, 0, 2, 2);
    print_mac(x);

    ArpCache arp_cache;

    EthernetHandler eth_handler(tap_mac, tap);
    ARPHandler  arp_handler(tap_mac, ip, eth_handler, arp_cache);
    IPHandler   ip_handler(ip, eth_handler, arp_cache);
    ICMPHandler icmp_handler(ip_handler);
    TCPHandler  tcp_handler(ip_handler);

    eth_handler.register_protocol(0x0806, arp_handler);
    eth_handler.register_protocol(0x0800, ip_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::ICMP), icmp_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::TCP),  tcp_handler);

    int ret;
    int listen_fd = tcp_handler.tcp_socket();

    ret = tcp_handler.tcp_bind(listen_fd, 80);
    if (ret<0) throw std::runtime_error("Couldnt bind port to socket");

    ret = tcp_handler.tcp_listen(listen_fd);
    if (ret<0) throw std::runtime_error("Couldnt create listen socket");
    
    loop.add_event(tapfd,     EPOLLIN);
    loop.add_event(listen_fd, EPOLLIN);

    Router router{};
    router.add_route({HttpMethod::Get, "/hi"}, [](const Request&) {
        Response r;
        r.body = "Hello\n";
        return r;
    });

    // outside the loop — track active client fds
    std::unordered_map<int, HTTPConnection> conns;

    while(true){
        int n = loop.poll();

        for(int i = 0; i < n; i++){
            epoll_event event = loop.get_event(i);
            int fd = event.data.fd;
            if (fd == tapfd){
                auto buff_u = create_buffer();
                pkt_buff* buff = buff_u.get();
                if(!buff) continue;

                ssize_t bytes_read = tap.recv(buff->data, (buff->end - buff->data));
                if (bytes_read <= 0){
                    continue;
                }

                buff->tail += bytes_read;
                eth_handler.handle_packet(buff);
            } else if (fd == listen_fd){
                int client_fd = tcp_handler.tcp_accept(listen_fd);
                if (client_fd >=0){
                    loop.add_event(client_fd, EPOLLIN);
                    
                    HTTPConnection conn{};
                    conns[client_fd] = conn;
                }
            }else if (conns.find(fd) != conns.end()) {
                char buf[4096];
                auto &conn = conns[fd];
                ssize_t nr = tcp_handler.tcp_recv(fd, buf, sizeof(buf));
                if (nr == 0) {
                    close(fd);
                    conns.erase(fd);
                    continue;
                }
                if (nr < 0) {
                    if (errno == EAGAIN)
                        continue;
                    close(fd);
                    conns.erase(fd);
                    continue;
                }
                conn.read_buf.append(buf, static_cast<size_t>(nr));

                pump_connection(conn, router);

                if (conn.state == HTTPState::WRITING){

                    tcp_handler.tcp_send(fd, conn.write_buf.c_str(), conn.write_buf.size());

                    conn.write_buf.clear();
                    conn.state = HTTPState::CLOSED;
                    close(fd);
                    conns.erase(fd);
                }
            }
        }
    }
}