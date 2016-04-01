////////////////////////////////////////////////////////////////////////////////
// proxy.h
// author: jcramb@gmail.com

#ifndef proxy_h
#define proxy_h

#include "core.h"
#include "sock.h" 

#define PROXY_BUFSIZE     (8192)
#define PROXY_LISTENING   (1)
#define PROXY_PENDING     (2)
#define PROXY_ESTABLISHED (3)

////////////////////////////////////////////////////////////////////////////////
// tcp proxy routed through a transport module

class transport_proxy {
public:
    transport_proxy();
    ~transport_proxy();

    int enable(int s_port, std::string d_ip, int d_port);
    void disable(int s_port);

    int poll(transport & tpt, int timeout_ms = 0);
    int handle_msg(transport & tpt, message & msg);
    void close(int s_port = -1);

protected:
    std::map<int, std::shared_ptr<tcp_stream>> m_upstreams;
    std::map<int, std::shared_ptr<tcp_stream>> m_downstreams;
    std::map<int, std::shared_ptr<sock_info>> m_headers;
    std::map<int, int> m_state;

    void build_sockset(fd_set & socks, int & fd_max);
    int dispatch_data(transport & tpt, int sock, int s_port,
                      std::shared_ptr<tcp_stream> & stream);
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
