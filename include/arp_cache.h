#pragma once

#include "net_addr.h"

#include <optional>
#include <unordered_map>

class ArpCache {
public:
    void learn(const ip4_addr_t& ip, const mac_addr_t& mac) {
        table_[ip] = mac;
    }

    std::optional<mac_addr_t> lookup(const ip4_addr_t& ip) const {
        auto it = table_.find(ip);
        if (it == table_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::unordered_map<ip4_addr_t, mac_addr_t> table_;
};
