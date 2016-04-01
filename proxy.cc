////////////////////////////////////////////////////////////////////////////////
// proxy.cc
// author: jcramb@gmail.com

#include "proxy.h"

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>

#include "core.h"

////////////////////////////////////////////////////////////////////////////////
// ctors / dtors

proxy_listener::proxy_listener() {

}

proxy_listener::~proxy_listener() {

}

////////////////////////////////////////////////////////////////////////////////

int proxy_listener::enable(int s_port, std::string d_ip, int d_port) {

    // can only have one proxy route per port
    if (m_streams.find(s_port) != m_streams.end()) {
        LOG("proxy: (error) already active for port %d\n", s_port);
        return -1;
    }

    // bind to local port and store stream
    std::shared_ptr<tcp_stream> stream(new tcp_stream());
    if (stream->bind(s_port) < 0) {
        LOG("proxy: unable to bind to port %d\n", s_port);
        return -1;
    }

    // initialise proxy header information
    std::shared_ptr<sock_info> header(
        new sock_info(stream->src_ip(), s_port, d_ip, d_port)
    );

    // only makes sense to proxy a single connection
    stream->conn_limit(1);

    // add proxy route to data structures
    m_headers[s_port] = header;
    m_streams[s_port] = stream;
    LOG("proxy: routing %s:%d to %s:%d\n", 
        stream->src_ip(), s_port, d_ip.c_str(), d_port);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

void proxy_listener::disable(int s_port) {
    m_streams.erase(s_port);
    m_headers.erase(s_port);
}

////////////////////////////////////////////////////////////////////////////////

int proxy_listener::poll(transport & tpt, int timeout_ms) {
    if (m_streams.empty()) return 0;
    char buf[PROXY_BUFSIZE];

    fd_set socks;
    int fd_max = -1;
    int bytes;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = timeout_ms * 1000;

    FD_ZERO(&socks);
    for (auto & kv : m_streams) {
        std::shared_ptr<tcp_stream> stream = kv.second;

        // add stream socks to check for incoming connections
        fd_max = MAX(fd_max, stream->sock());
        FD_SET(stream->sock(), &socks);

        // add all client socks to check for incoming data
        for (int sock : stream->client_socks()) {
            fd_max = MAX(fd_max, sock);
            FD_SET(sock, &socks);
        }
    }

    // poll sockets 
    if (select(fd_max + 1, &socks, 0, 0, &tv) < 0) {
        LOG("proxy: failed to poll sockets\n");
        return -1;
    }
                    
    // iterate through proxy streams
    for (auto & kv : m_streams) {
        std::shared_ptr<tcp_stream> stream = kv.second;

        // accept incoming client connections
        if (FD_ISSET(stream->sock(), &socks)) {
            int sock = stream->accept();
            if (sock != SOCK_LIMIT && sock < 0) {
                return -1;
            } else if (sock != SOCK_LIMIT) {

                // configure socket and add to listener
                sock_set_blocking(sock, false);
                LOG("proxy: client connected (%d) on port (%d)\n", 
                        sock, stream->src_port());
            }
        }

        // iterate through proxy stream client connections
        for (int sock : stream->client_socks()) { 

            // does current client socket have data?
            if (FD_ISSET(sock, &socks)) {

                // read proxy buffer data from local socket 
                int bytes_ready = stream->recv(buf, PROXY_BUFSIZE, sock);
                if (bytes_ready <= 0) {
                    stream->close(sock);
                    return -1;
                }

                char * src = buf;
                int hlen = sizeof(sock_info);
                int s_port = stream->src_port();
                message msg(MSG_PROXYME); 

                // add proxy header information
                std::shared_ptr<sock_info> header = m_headers[s_port];
                strncpy(header->s_ip, stream->dst_ip(), sizeof(header->s_ip));
                memcpy(msg.body(), header.get(),  hlen);

                while (bytes_ready > 0) {

                    // determine size of message
                    msg.resize(hlen + bytes_ready);
                    int len = msg.body_len() - hlen; 

                    // fill message with proxy buffer data
                    memcpy(msg.body() + hlen, src, len); 

                    // update read buffer info for next message (if req'd)
                    bytes_ready -= len;
                    src += len;

                    // relay proxy msg to remote system
                    if (tpt.send(msg) < 0) {
                        LOG("proxy: failed to deliver message!\n");
                        this->close();
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

int proxy_listener::handle_msg(message & msg) {
    sock_info * header = (sock_info*)msg.body();
    
    // check that the message relates to a valid route
    if (m_streams.find(header->d_port) == m_streams.end()) {
        LOG("proxy: message for invalid downstream port\n");
        return -1;
    }

    // forward msg bytes to local client connection
    char * buf = msg.body() + sizeof(sock_info);
    int len = msg.body_len() - sizeof(sock_info); 
    int bytes = m_streams[header->d_port]->send(buf, len); 
    return bytes;
}

////////////////////////////////////////////////////////////////////////////////

void proxy_listener::close() {
    LOG("proxy: all routes closed\n");
    m_streams.clear();
    m_headers.clear();
}

////////////////////////////////////////////////////////////////////////////////
// ctors / dtors

tcp_proxy::tcp_proxy() {
    m_active = false;
};

tcp_proxy::~tcp_proxy() {
    this->close();
}

////////////////////////////////////////////////////////////////////////////////

int tcp_proxy::bind(int s_port) {

    // bind to local port
    LOG("proxy: binding to port %d\n", s_port);
    if (m_downstream.bind(s_port) < 0) {
        this->close();
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

int tcp_proxy::establish(std::string d_ip, int d_port) {

    // reset any previous poll state
    u_len = d_len = 0;

    // wait for client connection
    LOG("proxy: waiting for connection\n");
    if ((d_sock = m_downstream.accept()) < 0) {
        this->close();
        return -1;
    }

    // establish downstream connection
    LOG("proxy: connecting downstream %s:%d\n", d_ip.c_str(), d_port);
    if ((u_sock = m_upstream.connect(d_ip, d_port)) < 0) {
        this->close();
        return -1;
    }

    // set non-blocking ports
    sock_set_blocking(u_sock, false);
    sock_set_blocking(d_sock, false);
    m_active = true;
}

////////////////////////////////////////////////////////////////////////////////

int tcp_proxy::poll() {
    if (!m_active) return -1;

    fd_set socks;
    int fd_max = MAX(u_sock, d_sock); 
    int bytes;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;

    if (u_len) {
        bytes = write(d_sock, u_buf, u_len);
        if (bytes < 0 && errno != EWOULDBLOCK) {
            this->close();
            return -1;
        }
        if (bytes != u_len) {
            memmove(u_buf, u_buf + bytes, u_len - bytes); 
        }
        u_len -= bytes;
    }

    if (d_len) {
        bytes = write(u_sock, d_buf, d_len);
        if (bytes < 0 && errno != EWOULDBLOCK) {
            this->close();
            return -1;
        }
        if (bytes != d_len) {
            memmove(d_buf, d_buf + bytes, d_len - bytes); 
        }
        d_len -= bytes;
    }

    FD_ZERO(&socks);
    if (u_len < PROXY_BUFSIZE) FD_SET(u_sock, &socks);
    if (d_len < PROXY_BUFSIZE) FD_SET(d_sock, &socks);

    int result = select(fd_max + 1, &socks, 0, 0, &tv); 
    if (result > 0) {
    
        if (FD_ISSET(u_sock, &socks)) {
            bytes = read(u_sock, u_buf + u_len, PROXY_BUFSIZE - u_len);         
            if (bytes > 0) {
                u_len += bytes;
            } else {
                this->close();
                return -1;
            }
        }
        
        if (FD_ISSET(d_sock, &socks)) {
            bytes = read(d_sock, d_buf + d_len, PROXY_BUFSIZE - d_len);         
            if (bytes > 0) {
                d_len += bytes;
            } else {
                this->close();
                return -1;
            }
        }

    } else if (result < 0 && errno != EINTR) {
        this->close();
        return -1;
    }
    
    return 0;
}

////////////////////////////////////////////////////////////////////////////////

void tcp_proxy::close() {
    LOG("proxy: closing connections\n");
    m_active = false;
    m_upstream.close();
    m_downstream.close();
}

////////////////////////////////////////////////////////////////////////////////
