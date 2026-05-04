#include <iostream>
#include <unistd.h> // for read write open files
#include <cstring> // cpp version of string
#include <netinet/in.h> // IP related stuff
#include <sys/socket.h> // syscalls for interacting with sockets



int main(){

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

    std::cout << "Server Listening on Port 8080... \n";

    // while true - blocking
    while(true){
        // accept connection
        int client_fd = accept(server_fd, nullptr, nullptr);
        // addr and addr_len is used to fetch client's ip addr info
        if (client_fd < 0){
            perror("accept failed");
            continue;
        }

        // read from buffer
        char buffer[4096] = {0}; // set all values of array to 0 
        int bytes_read = read(client_fd, buffer, sizeof(buffer));

        std::cout<< "Bytes Read: "<< bytes_read << "\n"; 
        std::cout << "Request: \n"<< buffer << "\n";

        // process byte stream 

        // write back to buffer
        const char* body = "Hello World";
        int body_len = strlen(body);

        std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body_len) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + std::string(body);

        int sent = write(client_fd, response.c_str(), response.size());

        std::cout<< "Sent Bytes: "<< sent << "\n";

        close(client_fd);

        
    }
    close(server_fd);
    return 0;


}



