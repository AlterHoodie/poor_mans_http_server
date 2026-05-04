
#pragma once

#include <fcntl.h> // file control syscall, used to update flags on file descriptors
#include <sys/epoll.h>
#include <string>

void set_non_blocking(int fd);

void add_epoll_event(int &epfd, uint32_t events, int &dfd);

void modify_epoll_event(int &epfd, uint32_t events, int &dfd);


struct Connection{
    int fd;
    std::string read_buf;
    std::string write_buf;

    bool request_complete = false;
    bool response_ready = false;
};