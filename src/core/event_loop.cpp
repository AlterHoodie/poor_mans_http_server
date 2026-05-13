#include "event_loop.h"

#include <cstdint>
#include <stdexcept>

#include <sys/epoll.h>
#include <unistd.h>

EventLoop::EventLoop(){
    epfd_ = epoll_create1(0);
    if (epfd_==-1){
        throw std::runtime_error("Failed to Create event loop");
    }
}

EventLoop::~EventLoop(){
    close(epfd_);
}

void EventLoop::add_event(int &fd, uint32_t events){
    epoll_event ev{};

    ev.events = events;
    ev.data.fd = fd;

    int ret = epoll_ctl(
        epfd_,
        EPOLL_CTL_ADD,
        fd,
        &ev
    );

    if (ret<0){
        throw std::runtime_error("Failed to add event to epoll");
    }
}   

void EventLoop::modify_event(int &fd, uint32_t events){
    epoll_event ev{};

    ev.events = events;
    ev.data.fd = fd;

    int ret = epoll_ctl(
        epfd_,
        EPOLL_CTL_MOD, 
        fd, 
        &ev
    );

    if (ret<0){
        throw std::runtime_error("Failed to modify fd");
    }
}

void EventLoop::delete_event(int &fd){
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
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

epoll_event EventLoop::get_event(int n){
    if (n>EPOLL_BUFF_SIZE){
        throw std::runtime_error("n larger than event buff size");
    }

    return events_[n];
}