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

Tap::Tap(const std::string& ifname): fd_(-1),ifname_(ifname)
{
    setupTap();
    setNonBlocking();
}

Tap::~Tap(){
    if(fd_>=0){
        close(fd_);
    }
}

void Tap::setupTap(){
    fd_ = open("/dev/net/tun", O_RDWR);

    if (fd_<0){
        throw std::runtime_error("failed to open /dev/net/tun");
    }

    struct ifreq ifr {};

    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    std::strncpy(
        ifr.ifr_name,
        ifname_.c_str(),
        IFNAMSIZ
    );

    if (ioctl(fd_, TUNSETIFF, &ifr) < 0){
        close(fd_);
        throw std::runtime_error("TUNSETIFF failed");
    }

    ifname_.assign(ifr.ifr_name);
    readMacAddr();
}

void Tap::setNonBlocking(){
    int flags = fcntl(fd_, F_GETFL, 0);

    if (flags < 0) {
        throw std::runtime_error("fcntl(F_GETFL) failed");
    }

    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl(F_SETFL) failed");
    }
}

ssize_t Tap::recv(uint8_t* buf, size_t len){
    return read(fd_, buf, len);
}

int Tap::fd() const{
    return fd_;
}

void Tap::readMacAddr(){
    const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0){
        throw std::runtime_error("socket(AF_INET, SOCK_DGRAM) failed for SIOCGIFHWADDR");
    }

    struct ifreq ifr {};

    std::strncpy(ifr.ifr_name, ifname_.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0){
        close(sock);
        throw std::runtime_error("SIOCGIFHWADDR failed");
    }

    close(sock);

    for (int i = 0; i < 6; ++i){
        mac_[static_cast<size_t>(i)] =
            static_cast<uint8_t>(ifr.ifr_hwaddr.sa_data[i]);
    }
}

ssize_t Tap::transmit(pkt_buff *buff){
    return write(fd_, buff->data, buff->len());
}