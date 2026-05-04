#pragma once

#include "http_types.h"

#include <string>
#include <unordered_map>

struct Request{
    HttpMethod method = HttpMethod::Unknown;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};