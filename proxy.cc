////////////////////////////////////////////////////////////////////////////////
// proxy.cc
// author: jcramb@gmail.com

#include "proxy.h"

#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>

#include "core.h"

////////////////////////////////////////////////////////////////////////////////

proxy_header mk_proxy_header(std::string s_ip, int s_port,
                             std::string d_ip, int d_port) {
    proxy_header header;
    strncpy(header.s_ip, s_ip.c_str(), sizeof(header.s_ip));
    strncpy(header.d_ip, d_ip.c_str(), sizeof(header.d_ip));
    header.s_port = s_port;
    header.d_port = d_port;
    return header;
}

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

    // bind to local port
    tcp_stream stream;
    LOG("proxy: binding to port %d\n", s_port);
    if (stream.bind(s_port) < 0) {
        stream.close();
        return -1;
    }
 
    // wait for client connection
    int sock;
    LOG("proxy: waiting for connection on %d\n", s_port);
    if ((sock = stream.accept()) < 0) {
        stream.close();
        return -1;
    }

    // configure proxy header
    std::string s_ip = sock_get_ip();
    proxy_header header = mk_proxy_header(s_ip, s_port, d_ip, d_port);

    // configure socket and add to listener
    sock_set_blocking(sock, false);
    m_streams.emplace(s_port, stream);
    m_headers.emplace(s_port, header);
    LOG("proxy: routing local:%d to %s:%d\n", s_port, d_ip.c_str(), d_port);
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
        tcp_stream & stream = kv.second;
        for (auto sock : stream.client_socks()) {
            fd_max = MAX(fd_max, sock);
            FD_SET(sock, &socks);
        }
    }
                    
    int result = select(fd_max + 1, &socks, 0, 0, &tv);
    if (result > 0) {

        // iterate through proxy streams
        for (auto & kv : m_streams) {
        tcp_stream & stream = kv.second;
        for (auto sock : stream.client_socks()) { 

            // check if current stream requires attention
            if (FD_ISSET(sock, &socks)) {

                // read proxy buffer data from local socket 
                int bytes_ready = read(sock, buf, PROXY_BUFSIZE);
                if (bytes_ready <= 0) {
                    LOG("proxy:: listener failed reading from socket\n");
                    this->close();
                    return -1;
                }

                char * src = buf;
                int hlen = sizeof(proxy_header);
                int s_port = stream.src_port();
                message msg(MSG_PROXYME); 

                // fill message with relevant proxy header
                proxy_header & header = m_headers[s_port];
                memcpy(msg.body(), &header, sizeof(header)); 

                while (bytes_ready > 0) {

                    // determine size of message
                    msg.resize(hlen + bytes_ready);
                    int len = msg.body_len() - hlen; 

                    // fill message with proxy buffer data
                    memcpy(msg.body() + sizeof(header), src, len); 

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
        }}

    } else if (result < 0 && errno != EINTR) {
        LOG("proxy: listener failed select polling\n");
        this->close(); 
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

int proxy_listener::handle_msg(message & msg) {
    proxy_header * header = (proxy_header*)msg.body();
    
    // check that the message relates to a valid route
    if (m_streams.find(header->d_port) == m_streams.end()) {
        LOG("proxy: message for invalid downstream port\n");
        return -1;
    }

    // forward msg bytes to local client connection
    char * buf = msg.body() + sizeof(proxy_header);
    int len = msg.body_len() - sizeof(proxy_header); 
    int bytes = m_streams[header->d_port].send(buf, len); 
    return bytes;
}

////////////////////////////////////////////////////////////////////////////////

void proxy_listener::close() {
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
