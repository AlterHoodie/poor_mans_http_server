#include "parser.h"
#include "http_types.h"
#include "request.h"

#include <algorithm>
#include <cstdlib>
#include <string>


Request parse_request(const std::string& raw) {
    Request req;
    std::string delimiter = "\r\n";

    size_t i = 0;

    // ---------- 1. Parse request line ----------
    size_t line_end = raw.find(delimiter, i);
    if (line_end == std::string::npos)
        return req;

    std::string request_line = raw.substr(i, line_end - i);
    i = line_end + delimiter.size();

    size_t s1 = request_line.find(' ');
    size_t s2 = s1 == std::string::npos
                     ? std::string::npos
                     : request_line.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos || s2 <= s1)
        return req;

    req.method = parse_http_method(request_line.substr(0, s1));
    req.path = request_line.substr(s1 + 1, s2 - s1 - 1);
    req.version = request_line.substr(s2 + 1);

    // ---------- 2. Parse headers ----------
    while (true) {
        size_t next = raw.find(delimiter, i);
        if (next == std::string::npos)
            break;

        std::string line = raw.substr(i, next - i);
        i = next + delimiter.size();

        // empty line = end of headers
        if (line.empty()) break;

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // trim leading space
        if (!value.empty() && value[0] == ' ')
            value.erase(0, 1);

        req.headers[key] = value;
    }

    // ---------- 3. Parse body ----------
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        int len = std::stoi(it->second);
        if (len > 0 && i < raw.size()) {
            size_t take = std::min(static_cast<size_t>(len), raw.size() - i);
            req.body = raw.substr(i, take);
        }
    } else {
        req.body = "";
    }

    return req;
}


std::string build_response_string(const Response &res) {
    std::string delimiter = "\r\n";

    std::string res_string;

    // Status line
    const std::string& phrase =
        res.status_text.empty() ? std::string(default_status_text(res.status_code))
                                : res.status_text;
    res_string += "HTTP/1.1 " + std::to_string(static_cast<int>(res.status_code)) +
                  " " + phrase + delimiter;

    // Required headers (serializer-owned)
    res_string += "Content-Length: " + std::to_string(res.body.size()) + delimiter;
    res_string += "Connection: close" + delimiter;

    // Custom headers
    for (const auto& [key, value] : res.headers) {
        if (key == "Content-Length" || key == "Connection") continue;
        res_string += key + ": " + value + delimiter;
    }

    // End headers
    res_string += delimiter;

    // Body
    res_string += res.body;

    return res_string;
}