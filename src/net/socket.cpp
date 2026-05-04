#include "socket.h"

void set_non_blocking(int fd){
    int flags = fcntl(fd, F_GETFL, 0);

    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


void add_epoll_event(int &epfd, uint32_t events, int &dfd){
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = dfd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, dfd, &ev);
}

void modify_epoll_event(int &epfd, uint32_t events, int &dfd){
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = dfd;

    epoll_ctl(epfd, EPOLL_CTL_MOD, dfd, &ev);
}