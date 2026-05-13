#include <cstdint>
#include <sys/epoll.h>

#include "arp.h"
#include "arp_cache.h"
#include "buff.h"
#include "eth.h"
#include "event_loop.h"
#include "icmp.h"
#include "ip.h"
#include "net_addr.h"
#include "tap.h"
#include "utils.h"

constexpr int TIMEOUT = 10;

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

    eth_handler.register_protocol(0x0806, &arp_handler);
    eth_handler.register_protocol(0x0800, &ip_handler);
    ip_handler.register_protocol(static_cast<uint8_t>(IPProto::ICMP), icmp_handler);

    loop.add_event(tapfd, EPOLLIN);

    while(true){
        int n = loop.poll();

        for(int i = 0; i < n; i++){
            epoll_event event = loop.get_event(i);

            if (event.data.fd == tapfd){
                pkt_buff* buff = create_buffer();
                ssize_t bytes_read = tap.recv(buff->data, (buff->end - buff->data));
                if (bytes_read <= 0){
                    delete_buffer(buff);
                    continue;
                }

                buff->tail += bytes_read;
                eth_handler.handle_packet(buff);
                delete_buffer(buff);
            }
        }
    }
}
