#pragma once

#include "buff.h"

#include <string>
#include <cstdint>
#include <unistd.h>

class Tap {
public:
    explicit Tap(const std::string& ifname);
    ~Tap();

    int fd() const;
    const uint8_t* mac() const { return mac_; }

    ssize_t recv(uint8_t* buf, size_t len);
    ssize_t transmit(pkt_buff *buff);

private:
    int fd_;
    std::string ifname_;
    uint8_t mac_[6]{};

    void setupTap();
    void setNonBlocking();
    void readMacAddr();
};