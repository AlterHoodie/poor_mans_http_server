#include <cerrno>
#include <iostream>
#include <unistd.h> // for read write open files
#include <cstring> // cpp version of string
#include <netinet/in.h> // IP related stuff
#include <sys/socket.h> // syscalls for interacting with sockets
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <errno.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_map>

#include "http/http_types.h"
#include "http/parser.h"
#include "http/request.h"
#include "router/router.h"
#include "net/socket.h"
#include "tick/tick.h"
#include "tick/tick_handler.h"

static volatile sig_atomic_t g_stop = 0;
static void handle_sigint(int) { g_stop = 1; }

static void print_udp_stats(const TickStats& stats, uint64_t bad_size) {
    std::cout << "\n--- UDP stats ---\n"
              << "ticks=" << stats.ticks.load(std::memory_order_relaxed)
              << "  gaps=" << stats.gaps.load(std::memory_order_relaxed)
              << "  last_seq=" << stats.last_seq
              << "  bad_size=" << bad_size << '\n';
}

constexpr int TIMEOUT = 10;

static size_t parse_content_length(const Request& req) {
    auto it = req.headers.find("Content-Length");
    if (it == req.headers.end())
        return 0;
    try {
        return static_cast<size_t>(std::stoul(it->second));
    } catch (...) {
        return 0;
    }
}

bool parse_keep_alive(const Request& req){
    auto it = req.headers.find("Connection");
    if (it!=req.headers.end() && it->second=="close"){
        return false;
    }
    return true;
}

static void pump_connection(Connection& conn, Router& router, int epfd, int fd) {
    if (conn.state == ConnState::WRITING || conn.state == ConnState::CLOSED)
        return;

    if (conn.state == ConnState::READING_HEADERS) {
        size_t header_end = conn.read_buf.find("\r\n\r\n");
        if (header_end == std::string::npos)
            return;
        conn.body_start = header_end + 4;
        Request hdr = parse_request(conn.read_buf.substr(0, conn.body_start));
        conn.content_length = parse_content_length(hdr);
        conn.state =
            conn.content_length > 0 ? ConnState::READING_BODY : ConnState::PROCESSING;
        conn.keep_alive = parse_keep_alive(hdr);
    }

    if (conn.state == ConnState::READING_BODY) {
        if (conn.read_buf.size() < conn.body_start + conn.content_length)
            return;
        conn.state = ConnState::PROCESSING;
    }

    if (conn.state == ConnState::PROCESSING) {
        Request req = parse_request(
            conn.read_buf.substr(0, conn.body_start + conn.content_length));
        Response res = router.route(req);

        if (conn.keep_alive) 
            res.headers["Connection"] = "keep-alive";
        conn.write_buf = build_response_string(res);
        conn.read_buf.clear();
        conn.state = ConnState::WRITING;
        modify_epoll_event(epfd, EPOLLOUT, fd);
    }
}

int tcp_server(){
    // Create EPoll Instance
    int epfd = epoll_create1(0);
    if (epfd == -1){
        perror("epoll_create1");
        return 1;
    }


    // Create TimerFd instance for checking timeout 
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd == -1){
        perror("timerfd_create");
        return 1;
    }

    itimerspec ts{};
    ts.it_interval.tv_sec = 1; // fire every 1s
    ts.it_value.tv_sec = 1; // first fire in 1s

    if (timerfd_settime(tfd, 0, &ts, nullptr) == -1) {
        perror("timerfd_settime");
        return 1;
    }

    add_epoll_event(epfd, EPOLLIN, tfd);

    // Create TCP Server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // AF_INET - user ipv4 protocol for network layer
    // SOCK_STREAM - reliable byte stream semantics
    // SOCK_DGRAM - unreliable semantics
    // protocol = 0, pick the default transport layer for this combo <AF_INET, SOCK_STREAM> which is TCP 
    if (server_fd == -1) {
        perror("Socket Creation Failed");
        return 1;
    }
    set_non_blocking(server_fd);

    // Register Server with Epoll
    add_epoll_event(epfd, EPOLLIN, server_fd);

    // Connection Map
    std::unordered_map<int, Connection> conns;


    // Bind Socket
    sockaddr_in addr{};
    // contains Network layer protocal, which addr, which port etc.
    // zero initialize all fields
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080); // port 8080
    addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr))<0){ // associate this socket with this ip and port
        perror("Socket Bind Failed");
        return 1;
    }

    // Listen on socket
    if (listen(server_fd,10)<0){ // backlog is 10 here meaning 10 conenctions will be queues before server start rejecting connections
        perror("listen failed");
        return 1;
    }

    // Initialize Router
    Router router;

    Handler handler = [](const Request&) {
        Response res;
        res.status_code = StatusCode::Ok;
        res.status_text = "OK";
        res.body = "Hello World\n";

        return res;
    };
    Handler post_handler = [](const Request& req){
        Response res;
        res.status_code = StatusCode::Ok;
        res.status_text = "OK";
        res.body = req.body;
        return res;
    };

    Handler slow_get_handler = [](const Request& req){
        Response res;
        int ms = 500;

        auto pos = req.path.find("?ms");
        if(pos != std::string::npos){
            try{
                ms = std::stoi(req.path.substr(pos+4));
            }catch(...){
                res.status_code = StatusCode::BadRequest;
                res.body = "";
                return res;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        
        res.status_code = StatusCode::Ok;
        res.status_text = "OK";
        res.body = "Sleepy";

        return res;
    };

    Handler big_get_handler = [](const Request& req){
        Response res;
        int kb = 1024;

        auto pos = req.path.find("?kb");
        if(pos != std::string::npos){
            try{
                kb = std::stoi(req.path.substr(pos+4));
            }catch(...){
                res.status_code = StatusCode::BadRequest;
                res.body = "";
                return res;
            }
        }

        res.status_code = StatusCode::Ok;
        res.body = std::string(static_cast<size_t>(kb), 'x');
        return res;
    };

    router.add_route({HttpMethod::Get, "/"}, handler);
    router.add_route({HttpMethod::Post, "/"}, post_handler);
    router.add_route({HttpMethod::Get, "/slow"}, slow_get_handler);
    router.add_route({HttpMethod::Get, "/big"}, big_get_handler);

    std::cout << "Server Listening on Port 8080... \n";
    epoll_event events[1024];

    // while true - blocking
    while(true){
        int n = epoll_wait(epfd, events, 1024, -1);

        for (int i=0; i<n; i++){
            int fd = events[i].data.fd;

            if (fd == server_fd){
                while (true){
                    int client_fd = accept(server_fd, nullptr, nullptr);
                    // accept connection
                    // addr and addr_len is used to fetch client's ip addr info
                    if (client_fd < 0){
                        if (errno == EAGAIN || errno == EWOULDBLOCK) 
                            break;

                        perror("accept");
                        break;
                    }
                    set_non_blocking(client_fd);

                    add_epoll_event(epfd, EPOLLIN, client_fd);

                    auto ins = conns.emplace(client_fd, Connection{});
                    ins.first->second.fd = client_fd;

                }
            } else if (fd == tfd){
                uint64_t expirations;
                if (read(tfd, &expirations, sizeof(expirations)) < 0)
                    continue;

                auto now = std::chrono::steady_clock::now();

                for (auto it = conns.begin(); it!=conns.end();){
                    auto& conn = it->second;
                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                        now - conn.last_active
                    ).count();
                
                    if (idle > TIMEOUT) {  // 10s idle timeout
                        conn.state = ConnState::CLOSED;
                        close(conn.fd);
                        it = conns.erase(it);
                    } else {
                        ++it;
                    }
                }  
            } else {
                if(events[i].events & EPOLLIN){
                    auto& conn = conns[fd];

                    char buffer[4096];

                    while (true){
                        // Reading from fd and storing result in Connections Read Buffer
                        ssize_t nr = read(fd, buffer, sizeof(buffer));

                        if (nr > 0){
                            conn.read_buf.append(buffer, static_cast<size_t>(nr));
                        } else if(nr == 0){
                            // if client closes connection - EOF
                            close(fd);
                            conns.erase(fd);
                            break;
                        }
                        else{
                            if(errno == EAGAIN || errno == EWOULDBLOCK) 
                                // EAGAIN or EWOULDBLOCK means there is nothing left to read in the buffer
                                break;
                            // some fall error occured
                            close(fd);
                            conns.erase(fd);
                            break;
                        }

                    }
                    auto it = conns.find(fd);
                    if (it == conns.end()) continue;
                    pump_connection(conn, router, epfd, fd);
                }
                if(events[i].events & EPOLLOUT){
                    auto& conn = conns[fd];

                    while (!conn.write_buf.empty()) {
                        // Writing whatever is there in write_buf of connection into the fd
                        ssize_t nw = write(fd, conn.write_buf.c_str(), conn.write_buf.size());
                    
                        if (nw > 0) {
                            conn.write_buf.erase(0, static_cast<size_t>(nw));
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;
                            close(fd);
                            conns.erase(fd);
                            break;
                        }
                    }

                    // Response Data has been written into the socket
                    if (conn.write_buf.empty()) {
                        if (conn.keep_alive){
                            //reset connection for next request
                            conn.read_buf.clear();
                            conn.write_buf.clear();

                            conn.state = ConnState::READING_HEADERS;
                            conn.content_length = 0;
                            conn.body_start = 0;

                            conn.last_active = std::chrono::steady_clock::now();

                            modify_epoll_event(epfd, EPOLLIN, fd);

                        }
                        else{
                            conn.state = ConnState::CLOSED;
                            close(fd);
                            conns.erase(fd);
                        }
                    }
                }
            }
        }


    }
    close(server_fd);
    return 0;
}

int udp_server(bool echo){
    std::signal(SIGINT, handle_sigint);

    int epfd = epoll_create1(0);
    if (epfd == -1){
        perror("epoll_create1");
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(server_fd == -1){
        perror("Socket Creation Failed");
        return 1;
    }
    set_non_blocking(server_fd);
    add_epoll_event(epfd, EPOLLIN, server_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0){
        perror("Socket Bind Failed");
        return 1;
    }

    epoll_event events[1024];
    TickStats stats{};
    uint64_t bad_size = 0;

    std::cout << "Server listening on port 9000"
              << (echo ? " [echo mode]" : "") << "...\n";

    std::thread reporter([&stats](){
        using clock = std::chrono::steady_clock;
        uint64_t prev = 0;
        auto prev_time = clock::now();
        while (!g_stop){
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = clock::now();
            uint64_t cur = stats.ticks.load(std::memory_order_relaxed);
            double elapsed = std::chrono::duration<double>(now - prev_time).count();
            double pps = (cur - prev) / elapsed;
            std::cout << "pps=" << static_cast<uint64_t>(pps)
                      << "  total_ticks=" << cur
                      << "  gaps=" << stats.gaps.load(std::memory_order_relaxed)
                      << '\n';
            prev = cur;
            prev_time = now;
        }
    });

    while (!g_stop){
        int n = epoll_wait(epfd, events, 1024, 500);
        if (n < 0){
            if (errno == EINTR) break;
            perror("epoll_wait");
            break;
        }

        for(int i = 0; i < n; i++){
            int fd = events[i].data.fd;

            if(fd == server_fd){
                while(true){
                    char buf[2048];
                    sockaddr_in peer{};
                    socklen_t peer_len = sizeof(peer);

                    ssize_t nr = recvfrom(server_fd, buf, sizeof(buf), 0,
                                         reinterpret_cast<sockaddr*>(&peer), &peer_len);

                    if(nr < 0){
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("recvfrom");
                        break;
                    }

                    if (static_cast<size_t>(nr) != sizeof(Tick)){
                        ++bad_size;
                        continue;
                    }

                    const Tick* tick = reinterpret_cast<const Tick*>(buf);
                    handle_tick(*tick, stats);

                    if (echo){
                        sendto(server_fd, buf, sizeof(Tick), 0,
                               reinterpret_cast<sockaddr*>(&peer), peer_len);
                    }
                }
            }
        }
    }

    reporter.join();
    reporter.join();
    print_udp_stats(stats, bad_size);
    close(server_fd);
    close(epfd);
    return 0;
}

int main(int argc, char* argv[]){
    if(argc < 2){
        std::cerr << "usage: " << argv[0] << " <tcp|udp> [echo]\n";
        return 1;
    }
    if (std::strcmp(argv[1],"tcp") == 0){
        return tcp_server();
    }
    if (std::strcmp(argv[1], "udp") == 0){
        bool echo = (argc >= 3 && std::strcmp(argv[2], "echo") == 0);
        return udp_server(echo);
    }

    std::cerr << "Unknown mode: " << argv[1] << "\n";
    return 1;
}



