#include "tap.h"
#include "buff.h"

#include <stdexcept>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <linux/if_tun.h>
#include <net/if.h>

Tap::Tap(const std::string& ifname): ifname_(ifname)
{
    setupTap();
    setNonBlocking();
}

void Tap::setupTap(){
    fd_.reset(::open("/dev/net/tun", O_RDWR));

    if (fd_.get() < 0){
        throw std::runtime_error("failed to open /dev/net/tun");
    }

    struct ifreq ifr {};

    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    std::strncpy(
        ifr.ifr_name,
        ifname_.c_str(),
        IFNAMSIZ
    );

    if (ioctl(fd_.get(), TUNSETIFF, &ifr) < 0){
        fd_.reset();
        throw std::runtime_error("TUNSETIFF failed");
    }

    ifname_.assign(ifr.ifr_name);
    readMacAddr();
}

void Tap::setNonBlocking(){
    int flags = fcntl(fd_.get(), F_GETFL, 0);

    if (flags < 0) {
        throw std::runtime_error("fcntl(F_GETFL) failed");
    }

    if (fcntl(fd_.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl(F_SETFL) failed");
    }
}

ssize_t Tap::recv(uint8_t* buf, size_t len){
    return read(fd_.get(), buf, len);
}

int Tap::fd() const{
    return fd_.get();
}

void Tap::readMacAddr(){
    unique_fd sock{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (sock.get() < 0){
        throw std::runtime_error("socket(AF_INET, SOCK_DGRAM) failed for SIOCGIFHWADDR");
    }

    struct ifreq ifr {};

    std::strncpy(ifr.ifr_name, ifname_.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock.get(), SIOCGIFHWADDR, &ifr) < 0){
        throw std::runtime_error("SIOCGIFHWADDR failed");
    }

    for (int i = 0; i < 6; ++i){
        mac_[static_cast<size_t>(i)] =
            static_cast<uint8_t>(ifr.ifr_hwaddr.sa_data[i]);
    }
}

ssize_t Tap::transmit(pkt_buff *buff){
    return write(fd_.get(), buff->data, buff->len());
}