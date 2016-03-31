////////////////////////////////////////////////////////////////////////////////
// sock.cc
// author: jcramb@gmail.com

#include "sock.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <cstring>
#include <cstdio>

#include "core.h"

#define SOCKET_BACKLOG 10

////////////////////////////////////////////////////////////////////////////////
// helper funcs

bool sock_is_blocking(int fd) {
    return fcntl(fd, F_GETFL) & O_NONBLOCK;
}

int sock_set_blocking(int fd, bool blocking) {
    int flags = fcntl(fd, F_GETFL);
    int nflags = blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK;
    return fcntl(fd, F_SETFL, nflags);
}

static inline void * get_in_addr(struct sockaddr * sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    } else {
        return &(((struct sockaddr_in6*)sa)->sin6_addr);
    }
}

const char * sock_get_ip(std::string iface) {
    struct ifaddrs *ifaddrs, *index;
    static char addr_str[INET_ADDRSTRLEN];

    // obtain address info
    getifaddrs(&ifaddrs);

    // loop through interfaces
    for (index = ifaddrs; index != NULL; index = index->ifa_next) {

        // only consider IPv4, check if it matches specified interface
        if (index->ifa_addr->sa_family == AF_INET &&
           (iface.empty() || iface == index->ifa_name)) {

            // convert address to presentable format
            void * sai = &((struct sockaddr_in*)index->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, sai, addr_str, INET_ADDRSTRLEN);
        }
    }

    freeifaddrs(ifaddrs);
    return addr_str;
}

////////////////////////////////////////////////////////////////////////////////
// tcp_stream ctors / dtors

tcp_stream::tcp_stream() {
    m_sock = -1;
    s_port = d_port = -1;
    s_ip = sock_get_ip();
}

tcp_stream::~tcp_stream() {
    this->close();
}

////////////////////////////////////////////////////////////////////////////////
// SSL implementation to send transport msg's 

int tcp_stream::send(const char * buf, int len) {
    int bytes_sent = 0;
    while (bytes_sent < len) {
        int bytes = ::send(m_sock, buf + bytes_sent, len - bytes_sent, 0);
        if (bytes < 0) {
            LOG("error: (send) %s\n", strerror(errno));
            break;
        }
        bytes_sent += bytes;
    }
    return bytes_sent;
}

////////////////////////////////////////////////////////////////////////////////
// common func - receive all bytes waiting on socket

int tcp_stream::recv(char * buf, int len) {
    int bytes;

    return bytes;
}

////////////////////////////////////////////////////////////////////////////////
// clean up stream details

void tcp_stream::close() {
    if (m_sock > 0) {
        ::close(m_sock);
    }
    m_sock = -1;
    s_port = d_port = -2;
    d_ip = "";
}

////////////////////////////////////////////////////////////////////////////////
// client func - establish connection to server

int tcp_stream::connect(std::string host, int port) {
    struct addrinfo hints, *server, *index;
    char addr_str[INET6_ADDRSTRLEN];
    int ai_result;

    // configure hints for tcp connection
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // convert port to string
    char _port[8] = {0};
    port = CLAMP(port, 0, 65535);
    snprintf(_port, sizeof(_port), "%d", port);
    
    // obtain address info for server
    if ((ai_result = getaddrinfo(host.c_str(), _port, &hints, &server)) != 0) {
        LOG("error: %s\n", gai_strerror(ai_result));
        return -1;
    }

    // loop through results, connect to first possible
    for (index = server; index != NULL; index = index->ai_next) {

        // try to open a tcp socket for this result
        if ((m_sock = socket(index->ai_family,
                             index->ai_socktype,
                             index->ai_protocol)) < 0) {
            LOG("error: (socket) %s\n", strerror(errno));
            continue;
        } 

        // connect to server using new socket
        if (::connect(m_sock, index->ai_addr, index->ai_addrlen) < 0) {
            LOG("error: (connect) %s\n", strerror(errno));
            this->close();
            continue;
        }

        // obtain presentable address of server
        inet_ntop(index->ai_family,
                  get_in_addr((struct sockaddr*)index->ai_addr),
                  addr_str,
                  sizeof(addr_str));

        // success!
        d_ip = host;
        d_port = port;
        LOG("info: connected to %s:%d\n", d_ip.c_str(), d_port); 
        break;
    }

    // clean up address info
    freeaddrinfo(server);
    return index != NULL ? m_sock : -1;
}

////////////////////////////////////////////////////////////////////////////////
// server func - bind and listen to a tcp port

int tcp_stream::bind(int port) {
    struct addrinfo hints, *ai, *index;
    int ai_result;
    
    // configure hints for tcp connection
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_PASSIVE;

    // convert port to string
    char _port[8] = {0};
    port = CLAMP(port, 0, 65535);
    snprintf(_port, sizeof(_port), "%d", port);
    
    // obtain address info for server port
    if ((ai_result = getaddrinfo(NULL, _port, &hints, &ai)) != 0) {
        LOG("error:: %s\n", gai_strerror(ai_result));
        return -1;
    }

    // loop through results and bind to first possible
    for (index = ai; index != NULL; index = index->ai_next) {

        // attempt to open socket
        if ((m_sock = socket(index->ai_family,
                             index->ai_socktype,
                             index->ai_protocol)) < 0) {
            LOG("error: (socket) %s\n", strerror(errno));
            continue;
        } 

        // avoid "address already in use" errors
        int optval = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

        // bind socket to server port
        if (::bind(m_sock, index->ai_addr, index->ai_addrlen) < 0) {
            LOG("error: (bind) %s\n", strerror(errno));
            continue;
        } 

        // start listening for connections
        if (::listen(m_sock, SOCKET_BACKLOG) < 0) {
            LOG("error: (listen) %s\n", strerror(errno));
            this->close();
            continue;
        } 

        // success!
        s_port = port;
        break;
    }
    
    freeaddrinfo(ai);
    return index != NULL ? m_sock : -1;
}

////////////////////////////////////////////////////////////////////////////////
// server func - accept client connection 

int tcp_stream::accept() {
    struct sockaddr_storage client;
    char client_ip[INET6_ADDRSTRLEN];
    socklen_t len = sizeof(client);
    int client_sock;

    // wait for client to connect
    if ((client_sock = ::accept(m_sock, (struct sockaddr*)&client, &len)) < 0) {
      LOG("error: (accept) %s\n", strerror(errno));
      return -1;
    }

    // request connected client information
    inet_ntop(client.ss_family,
              get_in_addr((struct sockaddr*)&client),
              client_ip, 
              INET6_ADDRSTRLEN);

    // store stream info
    d_ip = client_ip;
    d_port = ntohs(((struct sockaddr_in*)&client)->sin_port); 
    LOG("info: connection from %s:%d\n", d_ip.c_str(), d_port); 

    // store client socket
    m_client_socks.push_back(client_sock);
    return client_sock;
}

////////////////////////////////////////////////////////////////////////////////
