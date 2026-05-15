#pragma once
#include "unique_fd.h"

#include <cstdint>
#include <sys/epoll.h>

class EventLoop{
    private:
        unique_fd epfd_;
        static constexpr int EPOLL_BUFF_SIZE = 1024;
        epoll_event events_[EPOLL_BUFF_SIZE];
    public:
        EventLoop();

        int add_event(int &fd, uint32_t events);
        int modify_event(int &fd, uint32_t events);
        int delete_event(int &fd);
        
        int poll(int timeout = -1);
        const epoll_event& get_event(int n);
};  