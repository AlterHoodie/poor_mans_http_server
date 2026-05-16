#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <rte_common.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unordered_map>
#include <rte_eal.h>
#include <rte_common.h>

#include "arp.h"
#include "arp_cache.h"
#include "buff.h"
#include "eth.h"
#include "http_types.h"
#include "icmp.h"
#include "ip.h"
#include "net_addr.h"
#include "parser.h"
#include "request.h"
#include "response.h"
#include "dpdk.h"
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

int main(int argc, char *argv[]){
    int retval;

    retval = rte_eal_init(argc, argv);
	if (retval<0){
		rte_exit(EXIT_FAILURE, "EAL Failed to init\n");
	}
    Dpdk dpdk;
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

    int ret;
    // outside the loop — track active client fds
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

    ret = tcp_handler.tcp_bind(listen_fd, 80);
    if (ret<0) rte_exit(EXIT_FAILURE,"Couldnt bind port to socket");

    ret = tcp_handler.tcp_listen(listen_fd);
    if (ret<0) rte_exit(EXIT_FAILURE,"Couldnt create listen socket");

    tcp_handler.set_on_accept(
        listen_fd, 
        [&conns, &router, &tcp_handler]
        (int fd){
            conns[fd] = HTTPConnection{};
            tcp_handler.set_on_data(
                fd, 
                [&conns, &router, &tcp_handler, fd]
                (const uint8_t* data, size_t len) {

                auto& conn = conns[fd];
                
                conn.read_buf.append((const char*)data, len);
                pump_connection(conn, router);
                if (conn.state == HTTPState::WRITING) {
                    tcp_handler.tcp_send(fd, conn.write_buf.c_str(), conn.write_buf.size());
                    conn.write_buf.clear();
                    conn.state = HTTPState::CLOSED;
                    tcp_handler.tcp_close(fd);
                    conns.erase(fd);
                    }
                }
            ); 

            tcp_handler.set_on_close(
                fd, 
                [&conns, fd](){
                    conns.erase(fd);
                }   
            );
        }
    );

    while(true){
        auto pkt = dpdk.recv_pkt();
        if (pkt) eth_handler.handle_packet(pkt.get());
    }
}