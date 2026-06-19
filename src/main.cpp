#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <csignal>
#include <atomic>

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
#include "udp.h"
#include "utils.h"
#include "router.h"

static volatile sig_atomic_t g_stop = 0;
static void handle_sigint(int) { g_stop = 1; }

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
        conn.keep_alive = parse_keep_alive(hdr);
    }

    if (conn.state == HTTPState::READING_BODY){
        if(conn.read_buf.size() < conn.body_start + conn.content_length) return;
        conn.state = HTTPState::PROCESSING;
    }
    if (conn.state == HTTPState::PROCESSING){
        const size_t message_end = conn.body_start + conn.content_length;
        Request req = parse_request(conn.read_buf.substr(0, message_end));
        Response res = router.route(req);

        conn.write_buf = build_response_string(res, conn.keep_alive);
        conn.read_buf.erase(0, message_end);
        conn.state = HTTPState::WRITING;
    }
}

static int run_http_server() {
    EventLoop loop = EventLoop();
    Tap tap = Tap("tap0");
    int tapfd = tap.fd();
    const uint8_t* x = tap.mac();
    mac_addr_t tap_mac(x);
    ip4_addr_t ip(192, 168, 29, 12);
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
    router.add_route({HttpMethod::Post, "/hi"}, [](const Request& req) {
        Response r;
        r.body = req.body.empty() ? "OK\n" : req.body;
        return r;
    });

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
                int client_fd;
                while ((client_fd = tcp_handler.tcp_accept(listen_fd)) >= 0) {
                    loop.add_event(client_fd, EPOLLIN);
                    conns[client_fd] = HTTPConnection{};
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

                while (true) {
                    pump_connection(conn, router);
                    if (conn.state != HTTPState::WRITING) break;

                    tcp_handler.tcp_send(fd, conn.write_buf.c_str(), conn.write_buf.size());
                    conn.write_buf.clear();

                    if (!conn.keep_alive) {
                        conn.state = HTTPState::CLOSED;
                        close(fd);
                        conns.erase(fd);
                        break;
                    }

                    conn.state          = HTTPState::READING_HEADERS;
                    conn.content_length = 0;
                    conn.body_start     = 0;
                }
            }
        }
    }
}

static int run_udp_server(bool echo, int work_iters) {
    std::signal(SIGINT, handle_sigint);

    EventLoop loop = EventLoop();
    Tap tap = Tap("tap0");
    int tapfd = tap.fd();
    const uint8_t* x = tap.mac();
    mac_addr_t tap_mac(x);
    ip4_addr_t ip(192, 168, 29, 12);
    print_mac(x);

    ArpCache arp_cache;

    EthernetHandler eth_handler(tap_mac, tap);
    ARPHandler  arp_handler(tap_mac, ip, eth_handler, arp_cache);
    IPHandler   ip_handler(ip, eth_handler, arp_cache);
    ICMPHandler icmp_handler(ip_handler);
    UDPHandler  udp_handler(ip_handler);

    eth_handler.register_protocol(0x0806, arp_handler);
    eth_handler.register_protocol(0x0800, ip_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::ICMP), icmp_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::UDP),  udp_handler);

    udp_handler.udp_bind(9000);
    udp_handler.set_echo(echo);
    udp_handler.set_work_iters(work_iters);

    loop.add_event(tapfd, EPOLLIN);

    std::cout << "UDP server listening on port 9000"
              << (echo ? " [echo mode]" : "")
              << (work_iters > 0 ? " [work-iters=" + std::to_string(work_iters) + "]" : "")
              << "\n";

    std::thread reporter([&udp_handler]() {
        using clock = std::chrono::steady_clock;
        uint64_t prev = 0;
        auto prev_time = clock::now();
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = clock::now();
            uint64_t cur = udp_handler.stats.ticks.load(std::memory_order_relaxed);
            double elapsed = std::chrono::duration<double>(now - prev_time).count();
            double pps = (cur - prev) / elapsed;
            std::cout << "pps=" << static_cast<uint64_t>(pps)
                      << "  total_ticks=" << cur
                      << "  gaps=" << udp_handler.stats.gaps.load(std::memory_order_relaxed)
                      << "\n";
            prev = cur;
            prev_time = now;
        }
    });

    while (!g_stop) {
        int n = loop.poll();
        for (int i = 0; i < n; i++) {
            epoll_event ev = loop.get_event(i);
            int fd = ev.data.fd;
            if (fd != tapfd) continue;

            auto buff_u = create_buffer();
            pkt_buff* buff = buff_u.get();
            if (!buff) continue;

            ssize_t bytes_read = tap.recv(buff->data, buff->end - buff->data);
            if (bytes_read <= 0) continue;

            buff->tail += bytes_read;
            eth_handler.handle_packet(buff);
        }
    }

    reporter.join();

    std::cout << "\n--- UDP stats ---\n"
              << "ticks=" << udp_handler.stats.ticks.load(std::memory_order_relaxed)
              << "  gaps=" << udp_handler.stats.gaps.load(std::memory_order_relaxed)
              << "  last_seq=" << udp_handler.stats.last_seq
              << "  bad_size=" << udp_handler.bad_size << "\n";

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <tcp|udp> [echo] [work-iters]\n";
        return 1;
    }
    if (std::strcmp(argv[1], "tcp") == 0) {
        return run_http_server();
    }
    if (std::strcmp(argv[1], "udp") == 0) {
        bool echo = false;
        int work_iters = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "echo") == 0) {
                echo = true;
            } else {
                work_iters = std::atoi(argv[i]);
            }
        }
        return run_udp_server(echo, work_iters);
    }
    std::cerr << "unknown mode: " << argv[1] << "\n";
    return 1;
}
