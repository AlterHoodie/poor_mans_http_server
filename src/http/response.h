#pragma once

#include "http_types.h"

#include <unordered_map>
#include <string>

struct Response{
    StatusCode status_code = StatusCode::Ok;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};