////////////////////////////////////////////////////////////////////////////////
// tcp.c
// author: jcramb@gmail.com

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

#include "tcp.h"
#include "core.h"

#define SOCKET_BACKLOG 10

////////////////////////////////////////////////////////////////////////////////

static inline char *  itoa(int i) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d", i);
    return buf;
}

static inline bool is_blocking(int sockfd) {
    return fcntl(sockfd, F_GETFL) & O_NONBLOCK;
}

static inline int set_blocking(int sockfd, bool blocking) {
    int flags = fcntl(sockfd, F_GETFL);
    int nflags = blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK;
    return fcntl(sockfd, F_SETFL, nflags);
}

static inline void * get_in_addr(struct sockaddr * sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    } else {
        return &(((struct sockaddr_in6*)sa)->sin6_addr);
    }
}

////////////////////////////////////////////////////////////////////////////////
// get ip address of the specified interface

std::string get_ip(std::string iface) {
    struct ifaddrs *ifaddrs, *index;
    char addr_str[INET_ADDRSTRLEN];

    // obtain address info
    getifaddrs(&ifaddrs);

    // loop through interfaces
    for (index = ifaddrs; index != NULL; index = index->ifa_next) {

        // only consider IPv4, check if it matches specified interface
        if (index->ifa_addr->sa_family == AF_INET &&
           (iface.empty() || index->ifa_name == iface)) {

            // convert address to presentable format
            void * sai = &((struct sockaddr_in*)index->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, sai, addr_str, INET_ADDRSTRLEN);
        }
    }

    freeifaddrs(ifaddrs);
    return addr_str;
}

////////////////////////////////////////////////////////////////////////////////
// tcp stream for client / server 

tcp_stream::tcp_stream() {
    m_sock = -1;
    s_port = d_port = 0;
    s_ip = get_ip();
}

tcp_stream::~tcp_stream() {
    if (m_sock > 0) {
        ::close(m_sock);
    }
}

int tcp_stream::send(const char * buf, int len) {
    int bytes_sent = 0;

    // keep sending until all buf has gone through
    while (bytes_sent < len) {
        int bytes = ::send(m_sock, buf + bytes_sent, len - bytes_sent, 0);
        if (bytes == -1) {
            LOG("error: send_all failed!\n");
            return -1;
        } else {
            bytes_sent += bytes;
        }
    }

    return bytes_sent;
}

int tcp_stream::recv(char * buf, int len, bool waitall) {

    // how much data is waiting?
    int flags = waitall ? MSG_WAITALL : MSG_DONTWAIT;
    int bytes = ::recv(m_sock, buf, len, flags); 

    // MSG_DONTWAIT, MSG_PEEK, MSG_WAITALL
    // grab packet header
    // grab packet
    // process packet

    return bytes;
}

int tcp_stream::connect(std::string host, int port) {
    struct addrinfo hints, *server, *index;
    char addr_str[INET6_ADDRSTRLEN];
    int ai_result;
    int err;
    
    // configure hints for TCP connection
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // obtain address information for server
    if ((ai_result = getaddrinfo(host.c_str(),
                                 itoa(port),
                                 &hints, &server)) != 0) {
        LOG("getaddrinfo: %s\n", gai_strerror(ai_result));
        return ai_result;
    }

    // loop through results, connect to first possible
    for (index = server; index != NULL; index = index->ai_next) {

        // open a tcp socket
        if ((m_sock = socket(index->ai_family,
                             index->ai_socktype,
                             index->ai_protocol)) == -1) {
            LOG("socket: error (%d)\n", m_sock);
            continue;
        } 

        // connect to server on newly opened socket
        if ((err = ::connect(m_sock,
                             index->ai_addr,
                             index->ai_addrlen)) == -1) {
            LOG("connect: error (%d)\n", err);
            close();
            continue;
        }

        // obtain presentable address of server
        inet_ntop(index->ai_family,
                  get_in_addr((struct sockaddr*)index->ai_addr),
                  addr_str,
                  sizeof(addr_str));

        // set socket to be non-blocking
        if (set_blocking(m_sock, false) < 0) {
            LOG("fcntl: non-blocking error\n");
            continue;
        }

        // success!
        d_ip = host;
        d_port = port;
        break;
    }

    freeaddrinfo(server);
    return 0;
}

int tcp_stream::bind(int port) {
    struct addrinfo hints, *ai, *index;
    int ai_result;
    
    // configure hints for tcp connection
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;

    // obtain address info for server port
    if ((ai_result = getaddrinfo(NULL, itoa(port), &hints, &ai)) != 0) {
        LOG("getaddrinfo: %s\n", gai_strerror(ai_result));
        return ai_result;
    }

    // loop through results and bind to first possible
    for (index = ai; index != NULL; index = index->ai_next) {

        // attempt to open socket
        if ((m_sock = socket(index->ai_family,
                             index->ai_socktype,
                             index->ai_protocol)) == -1) {
            LOG("socket: error (%d)\n", m_sock);
            continue;
        } 

        // avoid "address already in use" errors
        int optval = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

        // bind socket to server port
        if (::bind(m_sock, index->ai_addr, index->ai_addrlen) == -1) {
            LOG("bind: could not bind to port\n");
            close();
            continue;
        } 

        // start listening for connections
        if (::listen(m_sock, SOCKET_BACKLOG) == -1) {
            LOG("listen: could not listen on port\n");
            close();
            continue;
        } 

        // success!
        s_port = port;
        break;
    }
    
    freeaddrinfo(ai);
    return (index != NULL) ? 0 : -1;
}

int tcp_stream::accept() {

    // vars for client connection handling
    struct sockaddr_storage client;
    socklen_t addrlen = sizeof(client);
    char client_ip[INET6_ADDRSTRLEN];

    // accept connection
    m_sock = ::accept(m_sock, (struct sockaddr*) &client, &addrlen);
    if (m_sock == -1) {
        LOG("listen: failed to accept client connection\n");
        close();
        return -1;
    }

    // display connection message
    inet_ntop(client.ss_family,
              get_in_addr((struct sockaddr*)&client),
              client_ip, 
              INET6_ADDRSTRLEN);

    d_ip = client_ip;
    d_port = -1; // TODO set this
    return 0;
}

void tcp_stream::close() {
    if (m_sock > 0) {
        ::close(m_sock);
    }
    m_sock = -1;
    d_ip = "";
    s_port = d_port = 0;
}

////////////////////////////////////////////////////////////////////////////////
