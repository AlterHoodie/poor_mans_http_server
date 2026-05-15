#include "event_loop.h"

#include <cstdint>
#include <stdexcept>

#include <sys/epoll.h>
#include <sys/types.h>
#include <unistd.h>

EventLoop::EventLoop(){
    epfd_ = epoll_create1(0);
    if (epfd_==-1){
        throw std::runtime_error("Failed to Create event loop");
    }
}

EventLoop::~EventLoop(){
    if (epfd_ >=0) close(epfd_);
}

int EventLoop::add_event(int &fd, uint32_t events){
    epoll_event ev{};

    ev.events = events;
    ev.data.fd = fd;

    return epoll_ctl(
        epfd_,
        EPOLL_CTL_ADD,
        fd,
        &ev
    );
}   

int EventLoop::modify_event(int &fd, uint32_t events){
    epoll_event ev{};

    ev.events = events;
    ev.data.fd = fd;

    return epoll_ctl(
        epfd_,
        EPOLL_CTL_MOD, 
        fd, 
        &ev
    );
}

int EventLoop::delete_event(int &fd){
    return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
}

int EventLoop::poll(int timeout){
    int ret = epoll_wait(
        epfd_,
        events_,
        EPOLL_BUFF_SIZE,
        timeout
    );
    if (ret<0){
        throw std::runtime_error("Failed to poll event loop");
    }

    return ret;
}

const epoll_event& EventLoop::get_event(int n){
    if (n>=EPOLL_BUFF_SIZE){
        throw std::runtime_error("n larger than event buff size");
    }

    return events_[n];
}