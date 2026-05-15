#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <ostream>
#include <span>
#include <sys/types.h>

struct mac_addr_t {
    std::array<std::uint8_t, 6> bytes{};

    mac_addr_t() = default;

    explicit mac_addr_t(const std::uint8_t* p) {
        std::copy_n(p, 6, bytes.begin());
    }

    explicit mac_addr_t(std::span<const std::uint8_t, 6> s) {
        std::copy(s.begin(), s.end(), bytes.begin());
    }

    constexpr mac_addr_t(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                         std::uint8_t e, std::uint8_t f) noexcept
        : bytes{a, b, c, d, e, f} {}

    [[nodiscard]] static constexpr mac_addr_t broadcast() noexcept {
        return mac_addr_t(0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes.data(); }
    [[nodiscard]] std::uint8_t* data() noexcept { return bytes.data(); }

    [[nodiscard]] std::span<const std::uint8_t, 6> as_span() const noexcept {
        return std::span<const std::uint8_t, 6>(bytes);
    }
    [[nodiscard]] std::span<std::uint8_t, 6> as_span() noexcept {
        return std::span<std::uint8_t, 6>(bytes);
    }

    auto operator<=>(const mac_addr_t&) const = default;

    auto operator=(const std::uint8_t* p){
        std::copy_n(p, 6, bytes.begin());
    }
};

struct ip4_addr_t {
    std::array<std::uint8_t, 4> bytes{};

    ip4_addr_t() = default;

    explicit ip4_addr_t(const std::uint8_t* p) noexcept { std::copy_n(p, 4, bytes.begin()); }

    explicit ip4_addr_t(std::span<const std::uint8_t, 4> s) noexcept {
        std::copy(s.begin(), s.end(), bytes.begin());
    }

    constexpr ip4_addr_t(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                         std::uint8_t d) noexcept
        : bytes{a, b, c, d} {}

    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes.data(); }
    [[nodiscard]] std::uint8_t* data() noexcept { return bytes.data(); }

    [[nodiscard]] std::span<const std::uint8_t, 4> as_span() const noexcept {
        return std::span<const std::uint8_t, 4>(bytes);
    }
    [[nodiscard]] std::span<std::uint8_t, 4> as_span() noexcept {
        return std::span<std::uint8_t, 4>(bytes);
    }

    auto operator<=>(const ip4_addr_t&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const mac_addr_t& m) {
    const auto f = os.flags();
    const auto fill = os.fill();
    for (std::size_t i = 0; i < m.bytes.size(); ++i) {
        if (i != 0) {
            os << ':';
        }
        os << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
           << static_cast<unsigned>(m.bytes[i]);
    }
    os.flags(f);
    os.fill(fill);
    os << std::dec << std::setfill(' ') << std::setw(0);
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const ip4_addr_t& a) {
    for (std::size_t i = 0; i < a.bytes.size(); ++i) {
        if (i != 0) {
            os << '.';
        }
        os << static_cast<unsigned>(a.bytes[i]);
    }
    return os;
}

enum class IPVersion : std::uint32_t {
    IPv4 = 0x00000004,
    IPv6 = 0x00000006,
};

struct ip_ver_t{
    uint8_t bytes;

    ip_ver_t() = default;

    explicit ip_ver_t(uint8_t* p) { bytes = (*p >> 4) & 0x0F;};

    [[nodiscard]] const std::uint8_t* data() const noexcept { return &bytes; }
    [[nodiscard]] std::uint8_t* data() noexcept { return &bytes; }

    [[nodiscard]] bool is_ip4() const noexcept {return bytes == 4;};
    [[nodiscard]] bool is_ip6() const noexcept {return bytes == 6;};
    
};

namespace std {

template <>
struct hash<mac_addr_t> {
    std::size_t operator()(const mac_addr_t& m) const noexcept {
        std::size_t h = 0;
        for (std::uint8_t b : m.bytes) {
            h = h * 131u + static_cast<std::size_t>(b);
        }
        return h;
    }
};

template <>
struct hash<ip4_addr_t> {
    std::size_t operator()(const ip4_addr_t& a) const noexcept {
        std::size_t h = 0;
        for (std::uint8_t b : a.bytes) {
            h = h * 131u + static_cast<std::size_t>(b);
        }
        return h;
    }
};

}  // namespace std
