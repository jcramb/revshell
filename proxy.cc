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

transport_proxy::transport_proxy() {
}

transport_proxy::~transport_proxy() {
    this->close();
}

////////////////////////////////////////////////////////////////////////////////
// add a proxy route from local port to remote ip/port

int transport_proxy::enable(int s_port, std::string d_ip, int d_port) {

    // can only have one proxy route per port
    if (m_downstreams.find(s_port) != m_downstreams.end()) {
        LOG("proxy: (error) already active for port %d\n", s_port);
        return -1;
    }

    // bind tcp listener to desired port
    std::shared_ptr<tcp_stream> stream(new tcp_stream());
    if (stream->bind(s_port) < 0) {
        LOG("proxy: unable to bind to port %d\n", s_port);
        return -1;
    }

    // configure proxy stream and setup header
    stream->conn_limit(1);
    std::shared_ptr<sock_info> header(
        new sock_info(stream->src_ip(), s_port, d_ip, d_port)
    );

    // add proxy route to data structures
    m_headers[s_port] = header;
    m_downstreams[s_port] = stream;
    m_state[s_port] = PROXY_LISTENING;
    LOG("proxy: adding route[%d] %s:%d to %s:%d\n", 
        stream->sock(), stream->src_ip(), s_port, d_ip.c_str(), d_port);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// remove a proxy route for a given port

void transport_proxy::disable(int s_port) {
    LOG("proxy: removing route (port %d)\n", s_port);
    m_downstreams.erase(s_port);
    m_headers.erase(s_port);
    m_state.erase(s_port);
}

////////////////////////////////////////////////////////////////////////////////
// helper func - build fd_set of proxy sockets that need polling

void transport_proxy::build_sockset(fd_set & socks, int & fd_max) {

    // add downstreams to socket set
    for (auto & kv : m_downstreams) {
        std::shared_ptr<tcp_stream> & stream = kv.second;
        int sock = stream->sock();
        int s_port = stream->src_port();

        // add listener socks to check for incoming connections
        if (m_state[s_port] == PROXY_LISTENING) {
            fd_max = MAX(fd_max, sock);
            FD_SET(sock, &socks);
        
        // add client socks to check for incoming data
        } else if (m_state[s_port] == PROXY_ESTABLISHED) {
            for (int sock : stream->client_socks()) {
                fd_max = MAX(fd_max, sock);
                FD_SET(sock, &socks);
            }
        }
    }

    // add upstreams to socket set
    for (auto & kv : m_upstreams) {
        std::shared_ptr<tcp_stream> & stream = kv.second;
        int sock = stream->sock();

        // add sock to check for incoming data
        fd_max = MAX(fd_max, sock);
        FD_SET(sock, &socks);
    }
}

////////////////////////////////////////////////////////////////////////////////
// helper func - transport tcp proxy data via messages

int transport_proxy::dispatch_data(transport & tpt, int sock, int s_port, 
                                   std::shared_ptr<tcp_stream> & stream) { 
    char buf[PROXY_BUFSIZE];

    // read proxy buffer data from local socket 
    int bytes_ready = stream->recv(buf, sizeof(buf), sock);
    if (bytes_ready <= 0) {
        this->close(s_port);
        return -1;
    }

    char * src = buf;
    int header_len = sizeof(sock_info);
    message msg(MSG_PROXY_DATA); 

    // add proxy header information
    std::shared_ptr<sock_info> & header = m_headers[s_port];

    // TODO: move this into a get_header func for centralised error handling
    if (header.get() == NULL) {
        LOG("proxy: invalid header for port %d\n", s_port);
        return -1;
    }

    strncpy(header->s_ip, stream->dst_ip(), sizeof(header->s_ip));
    memcpy(msg.body(), header.get(),  header_len);

    // break data into multiple msgs if required
    while (bytes_ready > 0) {

        // determine size of message
        msg.resize(header_len + bytes_ready);
        int len = msg.body_len() - header_len; 

        // fill message with proxy buffer data
        memcpy(msg.body() + header_len, src, len); 

        // update read buffer info for next message (if req'd)
        bytes_ready -= len;
        src += len;

        // relay proxy msg to remote system
        if (tpt.send(msg) < 0) {
            LOG("proxy: failed to dispatch message (port %d)\n", s_port);
            return -1;
        }
    }

    return (int)(src - buf);
}

////////////////////////////////////////////////////////////////////////////////
// accept incoming connections and forward outgoing traffic (non-blocking)

int transport_proxy::poll(transport & tpt, int timeout_ms) {
    if (m_downstreams.empty() && m_upstreams.empty()) return 0;
   
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000 * timeout_ms;

    // compile set of sockets that require polling
    fd_set socks;
    int fd_max = -1;
    FD_ZERO(&socks);
    build_sockset(socks, fd_max);

    // poll status of socket set
    if (select(fd_max + 1, &socks, 0, 0, &tv) < 0) {
        LOG("proxy: failed to poll socket set\n");
        // TODO: add handling to close problem sockets and try again
        //       otherwise they will never be removed and will ruin
        //       any attempt to continue handling other proxy streams
        return -1;
    }
                    
    // check downstreams
    int err = 0;
    for (auto & kv : m_downstreams) {
        std::shared_ptr<tcp_stream> & stream = kv.second;
        int s_port = stream->src_port();

        // accept incoming client connections
        if (FD_ISSET(stream->sock(), &socks)) {
            int client_sock = stream->accept();
            if (client_sock > 0) {

                // proxy requires client socket to be non-blocking
                sock_set_blocking(client_sock, false);

                // update state of proxy connection
                m_state[s_port] = PROXY_PENDING;
                LOG("proxy: client connected (%d) on port (%d)\n", 
                        client_sock, s_port);
                
                // establish proxy connection upstream
                message msg(MSG_PROXY_INIT, sizeof(sock_info)); 
                std::shared_ptr<sock_info> & header = m_headers[s_port];
                memcpy(msg.body(), header.get(), sizeof(sock_info));
                if (tpt.send(msg) < 0) {
                    err = -1;
                }

                // TODO: add timeout to pending proxy connections
            }
        }

        // iterate through proxy stream client connections
        for (int sock : stream->client_socks()) { 
            if (FD_ISSET(sock, &socks) && 
                dispatch_data(tpt, sock, s_port, stream) < 0) {
                err = -1;
                continue;
            }
        }
    }
    
    // check upstreams
    for (auto & kv : m_upstreams) {
        std::shared_ptr<tcp_stream> & stream = kv.second;
        int sock = stream->sock();
        if (FD_ISSET(sock, &socks) && 
            dispatch_data(tpt, sock, kv.first, stream) < 0) {
            LOG("reading sock %d\n", sock);
            err = -1;
            continue;
        }
    }

    return err;
}

////////////////////////////////////////////////////////////////////////////////
// process incoming message from proxy peer

int transport_proxy::handle_msg(transport & tpt, message & msg) {
    int err = 0;
    switch (msg.type()) {

        case MSG_PROXY_INIT: {

            // create new tcp stream
            std::shared_ptr<tcp_stream> stream(new tcp_stream());
            sock_info * header = (sock_info*)msg.body();

            // try to establish upstream connection 
            int sock = stream->connect(header->d_ip, header->d_port); 
            int msg_type = (sock > 0) ? MSG_PROXY_PASS : MSG_PROXY_FAIL;
            int s_port = header->s_port;
            int d_port = header->d_port;

            // construct response message and send downstream 
            message response(msg_type, sizeof(int));
            int * port = (int*)response.body();
            *port = s_port;

            if (sock < 0) {
                LOG("proxy: upstream connection to %s:%d failed for (%d)\n",
                    header->d_ip, d_port, s_port); 
                err = -1;
            } else {
    
                // create header information
                std::shared_ptr<sock_info> new_header(
                    new sock_info(header->d_ip, d_port, 
                                  header->s_ip, s_port)
                );

                // add upstream and it's header to proxy
                m_headers[d_port] = new_header;
                m_upstreams[d_port] = stream;
                LOG("proxy: new upstream connection to %s:%d for (%d)\n",
                    header->d_ip, d_port, s_port); 
            }

            tpt.send(response);
            break;
        }

        case MSG_PROXY_PASS: {
            int s_port = *((int*)msg.body());
            m_state[s_port] = PROXY_ESTABLISHED;
            LOG("proxy: upstream connection established (port %d)\n", s_port);
            break;
        }
        
        case MSG_PROXY_FAIL: {
            int s_port = *((int*)msg.body());
            LOG("proxy: upstream connection failed (port %d)\n", s_port);
            this->close(s_port);
            break;
        }
        
        case MSG_PROXY_DATA: {

            // extract info from message
            sock_info * header = (sock_info*)msg.body();
            char * buf = msg.body() + sizeof(*header);
            int len = msg.body_len() - sizeof(*header); 
            int d_port = header->d_port;
            int bytes;

            // check that the message relates to a valid route
            if (m_downstreams.find(d_port) != m_downstreams.end()) {
                LOG("proxy: delivering downstream data to %d\n", d_port);
                bytes = m_downstreams[d_port]->send(buf, len); 

            } else if (m_upstreams.find(d_port) != m_upstreams.end()) {
                LOG("proxy: delivering upstream data to %d\n", d_port);
                bytes = m_upstreams[d_port]->send(buf, len); 
            }

            break;
        }
    }

    return err;
}

////////////////////////////////////////////////////////////////////////////////
// shutdown the proxy

void transport_proxy::close(int s_port) {
    if (s_port > 0) {

        if (m_upstreams.find(s_port) != m_upstreams.end()) {
            m_upstreams.erase(s_port);
            m_headers.erase(s_port);
            m_state.erase(s_port); // upstreams don't have state atm
            LOG("proxy: closed upstream (port %d)\n", s_port);
        }
        if (m_downstreams.find(s_port) != m_downstreams.end()) {
            m_state[s_port] = PROXY_LISTENING;
            m_downstreams[s_port]->disconnect_clients();
            LOG("proxy: closed downstream (port %d)\n", s_port);
        }

    } else {
        LOG("proxy: shutting down, closing all streams\n");
        m_downstreams.clear();
        m_upstreams.clear();
        m_headers.clear();
        m_state.clear();
    }
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
