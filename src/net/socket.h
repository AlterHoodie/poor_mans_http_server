
#pragma once

#include <fcntl.h> // file control syscall, used to update flags on file descriptors
#include <sys/epoll.h>
#include <string>
#include <chrono>

void set_non_blocking(int fd);

void add_epoll_event(int &epfd, uint32_t events, int &dfd);

void modify_epoll_event(int &epfd, uint32_t events, int &dfd);

enum class ConnState {
    READING_HEADERS,
    READING_BODY,
    PROCESSING,
    WRITING,
    CLOSED
};

struct Connection {
    int fd;

    std::string read_buf;
    std::string write_buf;

    ConnState state = ConnState::READING_HEADERS;

    size_t content_length = 0;
    size_t body_start = 0; // where body begins in read_buf

    bool keep_alive = true;
    std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();
};
