#include <iostream>
#include <unistd.h> // for read write open files
#include <cstring> // cpp version of string
#include <netinet/in.h> // IP related stuff
#include <sys/socket.h> // syscalls for interacting with sockets
#include <sys/epoll.h>
#include <errno.h>

#include "http/parser.h"
#include "http/request.h"
#include "http/response.h"
#include "router/router.h"
#include "net/socket.h"

int main(){

    // Create EPoll Instance
    int epfd = epoll_create1(0);
    if (epfd == -1){
        perror("epoll_create1");
        return 1;
    }


    // Create Server
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

    Handler handler = [](const Request &req) {
        Response res;
        res.status_code = StatusCode::Ok;
        res.status_text = "OK";
        res.body = "Hello World\n";
        
        return res;
    };
    router.add_route({HttpMethod::Get, "/"}, handler);
    
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

                    conns[client_fd] = Connection{client_fd};
                    
                }
            }
            else{
                if(events[i].events & EPOLLIN){
                    auto& conn = conns[fd];

                    char buffer[4096];

                    while (true){
                        // Reading from fd and storing result in Connections Read Buffer
                        ssize_t n = read(fd, buffer, sizeof(buffer));

                        if (n > 0){
                            conn.read_buf.append(buffer,n);
                        } else if(n == 0){
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
                     // check if request complete
                    if (conn.read_buf.find("\r\n\r\n") != std::string::npos) {
                        Request req = parse_request(conn.read_buf);
                        Response res = router.route(req);
                    
                        conn.write_buf = build_response_string(res);
                    
                        // switch to write mode
                        modify_epoll_event(epfd, EPOLLOUT, fd);
                    }
                }
                if(events[i].events & EPOLLOUT){
                    auto& conn = conns[fd];

                    while (!conn.write_buf.empty()) {
                        // Writing whatever is there in write_buf of connection into the fd
                        ssize_t n = write(fd, conn.write_buf.c_str(), conn.write_buf.size());
                
                        if (n > 0) {
                            conn.write_buf.erase(0, n);
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
                        close(fd);
                        conns.erase(fd);
                    }
                }
            }
        }

        
    }
    close(server_fd);
    return 0;

}



