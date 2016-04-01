////////////////////////////////////////////////////////////////////////////////
// proxy.h
// author: jcramb@gmail.com

#ifndef proxy_h
#define proxy_h

#include "core.h"
#include "sock.h" 

#define PROXY_BUFSIZE 8192

////////////////////////////////////////////////////////////////////////////////
// tcp proxy client (downstream)

class proxy_listener {
public:
    proxy_listener();
    ~proxy_listener();

    int enable(int s_port, std::string d_ip, int d_port);
    void disable(int s_port);

    int poll(transport & tpt, int timeout_ms = 0);
    int handle_msg(message & msg);
    void close();

protected:
    std::map<int, std::shared_ptr<tcp_stream>> m_streams;
    std::map<int, std::shared_ptr<sock_info>> m_headers;
};

////////////////////////////////////////////////////////////////////////////////
// tcp proxy server (upstream)

class proxy_dispatch {
public:
    proxy_dispatch();
    ~proxy_dispatch();

    int poll(int timeout_ms = 0);
    int handle_msg(message & msg);
    void close();

protected:
};

////////////////////////////////////////////////////////////////////////////////
// basic tcp proxy (blocking, single route connection)

class tcp_proxy {
public:

    // ctors / dtors
    tcp_proxy();
    ~tcp_proxy();

    // proxy interface
    int bind(int s_port);
    int establish(std::string d_ip, int d_port);
    int poll();
    void close();

protected:
    bool m_active;

    // poll state
    int u_len, d_len;
    int u_sock, d_sock;
    char u_buf[PROXY_BUFSIZE];
    char d_buf[PROXY_BUFSIZE];

    // proxy streams
    tcp_stream m_upstream;
    tcp_stream m_downstream;
};

////////////////////////////////////////////////////////////////////////////////

#endif // proxy_h
