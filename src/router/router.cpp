#include "router.h"

void Router::add_route(const RouteKey& key, Handler handler) {
    routes[key] = std::move(handler);
}

static Response not_found() {
    Response res;
    res.status_code = StatusCode::NotFound;
    res.status_text = "Not Found";
    res.body = "Not Found\n";

    return res;
}

Response Router::route(const Request& req) {
    auto its = req.path.substr(0, req.path.find('?'));
    
    RouteKey key{req.method, its};
    auto it = routes.find(key);
    if (it != routes.end())
        return it->second(req);

    return not_found();
}
