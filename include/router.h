#pragma once
#include "http_types.h"
#include "request.h"
#include "response.h"

#include <functional>
#include <string>
#include <unordered_map>

using Handler = std::function<Response(const Request&)>;

struct RouteKey {
    HttpMethod method;
    std::string path;
};

inline bool operator==(const RouteKey& a, const RouteKey& b) {
    return a.method == b.method && a.path == b.path;
}

namespace std {
template <>
struct hash<RouteKey> {
    size_t operator()(const RouteKey& k) const noexcept {
        size_t h1 = std::hash<int>{}(static_cast<int>(k.method));
        size_t h2 = std::hash<std::string>{}(k.path);
        return h1 ^ (h2 << 1);
    }
};
} // namespace std

struct Router {
    std::unordered_map<RouteKey, Handler> routes;

    void add_route(const RouteKey& key, Handler handler);

    Response route(const Request& req);
};
