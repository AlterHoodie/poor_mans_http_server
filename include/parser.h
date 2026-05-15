#pragma once
#include "request.h"
#include "response.h"

// To deserialize a string to Request Object
Request parse_request(const std::string& raw);
// To Parse Content Length
size_t parse_content_length(const Request& req);

// To serialize a Response Object to a string
std::string build_response_string(const Response& res);