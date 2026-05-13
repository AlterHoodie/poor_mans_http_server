#pragma once
#include <cstdint>
#include <sys/epoll.h>

class EventLoop{
    private:
        int epfd_;
        static constexpr int EPOLL_BUFF_SIZE = 1024;
        epoll_event events_[EPOLL_BUFF_SIZE];
    public:
        EventLoop();
        ~EventLoop();

        void add_event(int &fd, uint32_t events);
        void modify_event(int &fd, uint32_t events);
        void delete_event(int &fd);
        
        int poll(int timeout = -1);
        epoll_event get_event(int n);
};  