#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <rte_common.h>
#include <rte_eal.h>
#include <csignal>

#include "arp.h"
#include "arp_cache.h"
#include "buff.h"
#include "dpdk.h"
#include "eth.h"
#include "http_types.h"
#include "icmp.h"
#include "ip.h"
#include "net_addr.h"
#include "parser.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "tcp.h"
#include "udp.h"
#include "utils.h"

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

// ── TCP / HTTP server ────────────────────────────────────────────────────────

static int run_http_server(Dpdk& dpdk) {
    const uint8_t* x = dpdk.mac();
    mac_addr_t mac(x);
    ip4_addr_t ip(192, 168, 29, 36);
    print_mac(x);

    buff_pool_init(dpdk.pool());

    ArpCache arp_cache;

    EthernetHandler eth_handler(mac, dpdk);
    ARPHandler  arp_handler(mac, ip, eth_handler, arp_cache);
    IPHandler   ip_handler(ip, eth_handler, arp_cache);
    ICMPHandler icmp_handler(ip_handler);
    TCPHandler  tcp_handler(ip_handler);

    eth_handler.register_protocol(0x0806, arp_handler);
    eth_handler.register_protocol(0x0800, ip_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::ICMP), icmp_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::TCP),  tcp_handler);

    std::unordered_map<int, HTTPConnection> conns;
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

    int listen_fd = tcp_handler.tcp_socket();

    int ret = tcp_handler.tcp_bind(listen_fd, 80);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Couldnt bind port to socket");

    ret = tcp_handler.tcp_listen(listen_fd);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Couldnt create listen socket");

    tcp_handler.set_on_accept(
        listen_fd,
        [&conns, &router, &tcp_handler](int fd) {
            conns[fd] = HTTPConnection{};
            tcp_handler.set_on_data(
                fd,
                [&conns, &router, &tcp_handler, fd](const uint8_t* data, size_t len) {
                    auto& conn = conns[fd];
                    conn.read_buf.append(reinterpret_cast<const char*>(data), len);

                    while (true) {
                        pump_connection(conn, router);
                        if (conn.state != HTTPState::WRITING) break;

                        tcp_handler.tcp_send(fd, conn.write_buf.c_str(), conn.write_buf.size());
                        conn.write_buf.clear();

                        if (!conn.keep_alive) {
                            conn.state = HTTPState::CLOSED;
                            tcp_handler.tcp_close(fd);
                            conns.erase(fd);
                            break;
                        }

                        conn.state          = HTTPState::READING_HEADERS;
                        conn.content_length = 0;
                        conn.body_start     = 0;
                    }
                });

            tcp_handler.set_on_close(fd, [&conns, &tcp_handler, fd]() {
                conns.erase(fd);
                tcp_handler.tcp_close(fd);
            });
        });

    constexpr uint16_t BURST = BURST_SIZE;
    pkt_buff burst[BURST];

    while (true) {
        uint16_t n = dpdk.recv_burst(burst, BURST);
        for (uint16_t i = 0; i < n; i++) {
            burst[i].ip_src[0] = burst[i].ip_dst[0] = 0;
            eth_handler.handle_packet(&burst[i]);
        }
        dpdk.free_burst(burst, n);
    }
}

// ── UDP echo server ──────────────────────────────────────────────────────────

static int run_udp_server(Dpdk& dpdk, bool echo, int work_iters) {
    std::signal(SIGINT, handle_sigint);

    const uint8_t* x = dpdk.mac();
    mac_addr_t mac(x);
    ip4_addr_t ip(192, 168, 29, 36);
    print_mac(x);

    buff_pool_init(dpdk.pool());

    ArpCache arp_cache;

    EthernetHandler eth_handler(mac, dpdk);
    ARPHandler  arp_handler(mac, ip, eth_handler, arp_cache);
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

    constexpr uint16_t BURST = BURST_SIZE;
    pkt_buff burst[BURST];

    while (!g_stop) {
        uint16_t n = dpdk.recv_burst(burst, BURST);
        for (uint16_t i = 0; i < n; i++) {
            burst[i].ip_src[0] = burst[i].ip_dst[0] = 0;
            eth_handler.handle_packet(&burst[i]);
        }
        dpdk.free_burst(burst, n);
    }

    reporter.join();

    std::cout << "\n--- UDP stats ---\n"
              << "ticks="    << udp_handler.stats.ticks.load(std::memory_order_relaxed)
              << "  gaps="   << udp_handler.stats.gaps.load(std::memory_order_relaxed)
              << "  last_seq=" << udp_handler.stats.last_seq
              << "  bad_size=" << udp_handler.bad_size << "\n";

    return 0;
}

// ── entry point ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // rte_eal_init consumes its own args (EAL flags before --).
    // After init, argc/argv point at our application arguments.
    int eal_argc = argc;
    char** eal_argv = argv;
    int retval = rte_eal_init(eal_argc, eal_argv);
    if (retval < 0)
        rte_exit(EXIT_FAILURE, "EAL Failed to init\n");

    // Application args follow the EAL args (after the -- separator).
    int app_argc = argc - retval;
    char** app_argv = argv + retval;

    if (app_argc < 2) {
        std::cerr << "usage: " << app_argv[0] << " <tcp|udp> [echo] [work-iters]\n";
        return 1;
    }

    Dpdk dpdk;

    if (std::strcmp(app_argv[1], "tcp") == 0) {
        return run_http_server(dpdk);
    }
    if (std::strcmp(app_argv[1], "udp") == 0) {
        bool echo = false;
        int work_iters = 0;
        for (int i = 2; i < app_argc; ++i) {
            if (std::strcmp(app_argv[i], "echo") == 0) {
                echo = true;
            } else {
                work_iters = std::atoi(app_argv[i]);
            }
        }
        return run_udp_server(dpdk, echo, work_iters);
    }

    std::cerr << "unknown mode: " << app_argv[1] << "\n";
    return 1;
}
