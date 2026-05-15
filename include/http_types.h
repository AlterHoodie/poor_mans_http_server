#pragma once

#include <string>
#include <cstdint>

enum class HttpMethod {
    Get,
    Post,
    Put,
    Delete,
    Patch,
    Head,
    Options,
    Unknown,
};

enum class StatusCode : int {
    Ok = 200,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    InternalServerError = 500,
};

inline HttpMethod parse_http_method(const std::string& token) {
    std::string upper;
    upper.reserve(token.size());
    for (unsigned char uc : token) {
        if (uc >= 'a' && uc <= 'z')
            upper.push_back(static_cast<char>(uc - 'a' + 'A'));
        else
            upper.push_back(static_cast<char>(uc));
    }
    if (upper == "GET")
        return HttpMethod::Get;
    if (upper == "POST")
        return HttpMethod::Post;
    if (upper == "PUT")
        return HttpMethod::Put;
    if (upper == "DELETE")
        return HttpMethod::Delete;
    if (upper == "PATCH")
        return HttpMethod::Patch;
    if (upper == "HEAD")
        return HttpMethod::Head;
    if (upper == "OPTIONS")
        return HttpMethod::Options;
    return HttpMethod::Unknown;
}

inline const char* default_status_text(StatusCode code) {
    switch (code) {
    case StatusCode::Ok:
        return "OK";
    case StatusCode::BadRequest:
        return "Bad Request";
    case StatusCode::Unauthorized:
        return "Unauthorized";
    case StatusCode::Forbidden:
        return "Forbidden";
    case StatusCode::NotFound:
        return "Not Found";
    case StatusCode::InternalServerError:
        return "Internal Server Error";
    }
    return "Unknown";
}


enum class HTTPState{
    READING_HEADERS,
    READING_BODY,
    PROCESSING,
    WRITING,
    CLOSED
};

struct HTTPConnection{
    HttpMethod method;
    std::string path; 
    HTTPState state {HTTPState::READING_HEADERS};
    uint16_t content_length;
    size_t body_start;

    std::string read_buf;
    std::string write_buf;

};