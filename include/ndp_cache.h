#pragma once

#include "net_addr.h"

#include <optional>
#include <unordered_map>

class NdpCache {
public:
    void learn(const ip6_addr_t& ip, const mac_addr_t& mac) {
        table_[ip] = mac;
    }

    std::optional<mac_addr_t> lookup(const ip6_addr_t& ip) const {
        auto it = table_.find(ip);
        if (it == table_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::unordered_map<ip6_addr_t, mac_addr_t> table_;
};
