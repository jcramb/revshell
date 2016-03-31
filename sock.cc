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
    m_type = SOCK_INVALID;
    m_sock = -1;
    s_port = d_port = -1;
    s_ip = sock_get_ip();
}

tcp_stream::~tcp_stream() {
    this->close();
}

////////////////////////////////////////////////////////////////////////////////
// common func - send binary buffer over a tcp socket

int tcp_stream::send(const char * buf, int len, int sock) {

    if (m_type == SOCK_CLIENT) {
        sock = m_sock;
    } else if (m_type == SOCK_SERVER) {
        if (sock == m_sock) {
            LOG("error: attempt to send over server listening sock!\n");
        } else if (sock <= 0) {
            LOG("error: attempt to send over invalid sock (%d)\n", sock);
        }
        return -1;
    }

    int bytes_sent = 0;
    while (bytes_sent < len) {
        int bytes = ::send(sock, buf + bytes_sent, len - bytes_sent, 0);
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

int tcp_stream::recv(char * buf, int len, int sock) {

    if (m_type == SOCK_CLIENT) {
        sock = m_sock;
    } else if (m_type == SOCK_SERVER && sock <= 0) {
        LOG("error: attempt to send over invalid sock (%d)\n", sock);
        return -1;
    }

    int bytes = ::recv(sock, buf, len, 0);
    if (bytes < 0) {
        LOG("error: failed to recv data\n");
        return -1;
    }

    return bytes;
}

////////////////////////////////////////////////////////////////////////////////
// clean up stream details

void tcp_stream::close(int sock) {

    // are we closing a client socket?
    if (m_client_socks.find(sock) != m_client_socks.end()) {
        m_client_socks.erase(sock);
        ::close(sock);

    // otherwise, are we meant to close everything?
    } else if (m_sock == sock || sock == -1) {

        if (m_sock > 0) {
            ::close(m_sock);
            m_sock = -1;
        }

        for (auto sock : m_client_socks) {
            ::close(sock);
        }

        m_type = SOCK_INVALID;
        m_client_socks.clear();
        s_port = d_port = -1;
        d_ip = "";
    
    // we may be trying to close an already closed socket if we get here
    } else {
        LOG("error: attempt to close invalid socket (%d)\n", sock);
    }
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
        m_type = SOCK_CLIENT;
        LOG("info: connected to %s:%d\n", d_ip.c_str(), d_port); 
        break;
    }

    // clean up address info
    freeaddrinfo(server);
    return index != NULL ? m_sock : -1;
}

////////////////////////////////////////////////////////////////////////////////
// server func - send data buffer across all active connections

int tcp_stream::broadcast(const char * buf, int len) {

    // verify that this is a server stream 
    if (m_type != SOCK_SERVER) {
        LOG("error: broadcast on a non-server tcp stream\n");
        return -1;
    }

    int err = 0;
    for (auto sock : m_client_socks) {
        int bytes = this->send(buf, len, sock);
        if (bytes < 0) {
            LOG("error: broadcast failed for sock (%d)[%d]\n", sock, bytes);
            err = bytes;
        }
    }
    return err;
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
        m_type = SOCK_SERVER;
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
    m_client_socks.emplace(client_sock);
    return client_sock;
}

////////////////////////////////////////////////////////////////////////////////
